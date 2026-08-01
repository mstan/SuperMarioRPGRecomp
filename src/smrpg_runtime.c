#include "smrpg_runtime.h"

#include "common_rtl.h"
#include "cpu_state.h"
#include "snes/cart.h"
#include "snes/dma.h"
#include "snes/interp_bridge.h"
#include "snes/ppu.h"
#include "snes/sa1.h"
#include "snes/saveload.h"
#include "snes/snes.h"
#include "snes/ws_shadow.h"
#include "widescreen.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

enum {
  kMasterClocksPerLine = 1364u,
  kLinesPerFrame = 262u,
  kVblankStartLine = 225u,
  kMasterClocksPerFrame = kMasterClocksPerLine * kLinesPerFrame,
  kFirstVblankMaster = kMasterClocksPerLine * kVblankStartLine,
  kMaximumSlicesPerFrame = 8192,
  kMaximumInterruptsPerFrame = 128,
};

static bool s_initialized;
static uint32_t s_resume_pc;
static uint64_t s_next_frame_master;
static unsigned s_host_frames;
static int s_last_lle_result = 1;
static uint8_t s_frame_hdmaen;
static bool s_loaded_runtime_state;

bool g_ws_active = false;
int g_ws_extra = 0;
static bool s_widescreen_hud = true;

enum {
  kSmrpgMapSubtiles = 128,
  kSmrpgShadowBaseTile = 256,
  kSmrpgShadowBasePixel = kSmrpgShadowBaseTile * 8,
  kSmrpgMapL1Offset = 0x10000,
  kSmrpgMapL2Offset = 0x12000,
  kSmrpgTilesL1Offset = 0x15000,
  kSmrpgTilesL2Offset = 0x16000,
};

typedef struct SmrpgFieldMap {
  bool valid;
  bool have_x_bounds;
  bool have_hint;
  bool have_scroll;
  uint8_t bound_x0;
  uint8_t bound_x1;
  uint8_t source_x;
  uint8_t source_y;
  uint8_t hint_x;
  uint8_t hint_y;
  uint8_t hint_mask;
  uint8_t physical_x;
  uint8_t physical_y;
  uint16_t last_hscroll;
  uint16_t last_vscroll;
  uint32_t assignment_signature;
  unsigned exact_score;
  unsigned signal_score;
} SmrpgFieldMap;

static SmrpgFieldMap s_field_map;

static uint16_t read_ram16(size_t offset) {
  return (uint16_t)(g_ram[offset] | ((uint16_t)g_ram[offset + 1] << 8));
}

static uint16_t smrpg_full_map_tile(int layer, unsigned x, unsigned y) {
  const size_t map_offset =
      layer == 0 ? kSmrpgMapL1Offset : kSmrpgMapL2Offset;
  const size_t tiles_offset =
      layer == 0 ? kSmrpgTilesL1Offset : kSmrpgTilesL2Offset;
  const unsigned metatile_x = x >> 1;
  const unsigned metatile_y = y >> 1;
  const size_t map_word =
      map_offset + ((size_t)metatile_y * 64 + metatile_x) * 2;
  const unsigned metatile = read_ram16(map_word) & 0x01ffu;
  const size_t tile_word =
      tiles_offset + (size_t)metatile * 8 +
      (size_t)(y & 1u) * 4 + (size_t)(x & 1u) * 2;
  return read_ram16(tile_word);
}

static bool smrpg_is_signal_tile(uint16_t tile) {
  return tile != 0x0000u && tile != 0x0100u &&
         tile != 0x0900u && tile != 0x2000u &&
         tile != 0x2100u;
}

static int smrpg_current_area(void);

static bool smrpg_read_field_x_bounds(void) {
  Sa1 *sa1 = g_snes && g_snes->cart ? g_snes->cart->sa1 : NULL;
  uint8_t *bounds = sa1 ? sa1_cpu_memory_ptr(sa1, 0, 0x3120u) : NULL;
  if (!bounds)
    return false;

  /*
   * SMRPG's field loader publishes the assignment's horizontal 16-pixel
   * mask in SA-1 IRAM $3120/$3124; the high edge was made exclusive by the
   * loader. The expanded maps use 8-pixel subtiles, hence the factor of two.
   * The vertical mask is in the field engine's wrapped/projected coordinate
   * space and must not be applied directly to expanded-map Y.
   */
  const unsigned x0 = (unsigned)bounds[0] * 2u;
  const unsigned x1 = (unsigned)bounds[4] * 2u;
  if (x1 < x0 + 16u || x1 > kSmrpgMapSubtiles)
    return false;
  /*
   * 0..64 is the loader's default unlocked full-map mask rather than a
   * per-room boundary. It is still safe to match: margin publication is
   * filtered to authored tiles connected to the visible viewport, so packed
   * neighboring rooms remain hidden across intervening voids.
   */
  s_field_map.have_x_bounds = true;
  s_field_map.bound_x0 = (uint8_t)x0;
  s_field_map.bound_x1 = (uint8_t)x1;
  return true;
}

static void smrpg_score_map_viewport(unsigned physical_tile_x,
                                     unsigned physical_tile_y,
                                     unsigned camera_x,
                                     unsigned camera_y,
                                     unsigned *exact_out,
                                     unsigned *signal_out) {
  unsigned exact = 0;
  unsigned signal = 0;
  for (int layer = 0; layer < 2; layer++) {
    const unsigned map_base = (unsigned)PPU_bgTilemapAdr(g_ppu, layer);
    for (unsigned y = 0; y < 28; y++) {
      for (unsigned x = 0; x < 32; x++) {
        const uint16_t actual =
            g_ppu->vram[
                (map_base + ((physical_tile_y + y) & 31u) * 32 +
                 ((physical_tile_x + x) & 31u)) &
                0x7fffu];
        const uint16_t expected =
            smrpg_full_map_tile(layer, camera_x + x, camera_y + y);
        if (actual == expected) {
          exact++;
          if (smrpg_is_signal_tile(actual)) signal++;
        }
      }
    }
  }
  *exact_out = exact;
  *signal_out = signal;
}

static uint32_t smrpg_assignment_signature(void) {
  uint32_t hash = 2166136261u;
  for (unsigned i = 0; i < 16; i++) {
    hash ^= g_ram[0x03c0u + i];
    hash *= 16777619u;
  }
  const int area = smrpg_current_area();
  hash ^= (uint32_t)area;
  hash *= 16777619u;
  return hash;
}

static int smrpg_current_area(void) {
  Sa1 *sa1 = g_snes && g_snes->cart ? g_snes->cart->sa1 : NULL;
  uint8_t *area = sa1 ? sa1_cpu_memory_ptr(sa1, 0, 0x3030u) : NULL;
  if (!area) return -1;
  return area[0] | ((int)area[1] << 8);
}

static bool smrpg_find_field_map(void) {
  const unsigned physical_tile_x =
      ((uint16_t)g_ppu->hScroll[0] >> 3) & 31u;
  const unsigned physical_tile_y =
      ((uint16_t)g_ppu->vScroll[0] >> 3) & 31u;
  const unsigned physical_x = physical_tile_x & 16u;
  const unsigned physical_y = physical_tile_y & 16u;
  unsigned best_exact = 0;
  unsigned best_signal = 0;
  unsigned best_x = 0;
  unsigned best_y = 0;
  const unsigned physical_offset_x = physical_tile_x - physical_x;
  const unsigned physical_offset_y = physical_tile_y - physical_y;
  if (!s_field_map.have_x_bounds ||
      s_field_map.bound_x1 - s_field_map.bound_x0 < 32u)
    return false;
  const unsigned search_x0 =
      s_field_map.bound_x0 > physical_offset_x
          ? s_field_map.bound_x0
          : physical_offset_x;
  const unsigned search_x1 = s_field_map.bound_x1 - 32u;
  const unsigned search_y0 = physical_offset_y;
  const unsigned search_y1 = kSmrpgMapSubtiles - 28u;
  if (search_x0 > search_x1 || search_y0 > search_y1)
    return false;

  if (s_field_map.have_hint && s_field_map.hint_mask == 3u) {
    const unsigned hinted_camera_x =
        (unsigned)s_field_map.hint_x + physical_offset_x;
    const unsigned hinted_camera_y =
        (unsigned)s_field_map.hint_y + physical_offset_y;
    if (hinted_camera_x >= search_x0 &&
        hinted_camera_x <= search_x1 &&
        hinted_camera_y >= search_y0 &&
        hinted_camera_y <= search_y1) {
      smrpg_score_map_viewport(
        physical_tile_x, physical_tile_y,
        hinted_camera_x, hinted_camera_y,
        &best_exact, &best_signal);
      best_x = hinted_camera_x;
      best_y = hinted_camera_y;
    }
  }

  if (best_exact < 1200 || best_signal < 64) {
    best_exact = 0;
    best_signal = 0;
    for (unsigned y = search_y0; y <= search_y1; y++) {
      for (unsigned x = search_x0; x <= search_x1; x++) {
        unsigned exact;
        unsigned signal;
        smrpg_score_map_viewport(
            physical_tile_x, physical_tile_y, x, y,
            &exact, &signal);
        if (exact > best_exact ||
            (exact == best_exact && signal > best_signal)) {
          best_exact = exact;
          best_signal = signal;
          best_x = x;
          best_y = y;
        }
      }
    }
  }

  /*
   * Score the complete visible 32x28 tile viewport across both layers
   * (1792 comparisons), not one 16x16 circular quadrant. SMRPG packs several
   * similar isometric rooms into one 128x128 expanded map; a local quadrant
   * can match the wrong repeated room even though the full viewport cannot.
   */
  s_field_map.valid = best_exact >= 1200 && best_signal >= 64;
  s_field_map.source_x = (uint8_t)(best_x - physical_offset_x);
  s_field_map.source_y = (uint8_t)(best_y - physical_offset_y);
  s_field_map.have_hint = false;
  s_field_map.physical_x = (uint8_t)physical_x;
  s_field_map.physical_y = (uint8_t)physical_y;
  s_field_map.exact_score = best_exact;
  s_field_map.signal_score = best_signal;
  if (s_field_map.valid) {
    fprintf(stderr,
            "[smrpg-ws] area=%d quadrant=(%u,%u)->(%u,%u) "
            "camera=(%u,%u) exact=%u/1792 signal=%u\n",
            smrpg_current_area(), physical_x, physical_y,
            s_field_map.source_x, s_field_map.source_y,
            best_x, best_y, best_exact, best_signal);
  }
  return s_field_map.valid;
}

static bool smrpg_prepare_field_shadow(void) {
  const uint32_t signature = smrpg_assignment_signature();
  if (!signature) {
    s_field_map.valid = false;
    return false;
  }

  if (signature != s_field_map.assignment_signature) {
    memset(&s_field_map, 0, sizeof(s_field_map));
    s_field_map.assignment_signature = signature;
  }
  if (!smrpg_read_field_x_bounds()) {
    s_field_map.valid = false;
    return false;
  }
  if (s_field_map.valid &&
      (s_field_map.source_x < s_field_map.bound_x0 ||
       s_field_map.source_x > (unsigned)s_field_map.bound_x1 - 16u))
    s_field_map.valid = false;
  if (g_ppu->hScroll[0] != g_ppu->hScroll[1] ||
      g_ppu->vScroll[0] != g_ppu->vScroll[1]) {
    s_field_map.valid = false;
    return false;
  }

  const unsigned physical_tile_x =
      ((uint16_t)g_ppu->hScroll[0] >> 3) & 31u;
  const unsigned physical_tile_y =
      ((uint16_t)g_ppu->vScroll[0] >> 3) & 31u;
  const unsigned physical_x = physical_tile_x & 16u;
  const unsigned physical_y = physical_tile_y & 16u;
  const bool quadrant_changed =
      physical_x != s_field_map.physical_x ||
      physical_y != s_field_map.physical_y;

  if (s_field_map.valid && quadrant_changed) {
      int delta_x =
          (int)(((uint16_t)g_ppu->hScroll[0] -
                 s_field_map.last_hscroll) &
                0x03ffu);
      int delta_y =
          (int)(((uint16_t)g_ppu->vScroll[0] -
                 s_field_map.last_vscroll) &
                0x03ffu);
      if (delta_x >= 512) delta_x -= 1024;
      if (delta_y >= 512) delta_y -= 1024;
      int hint_x = s_field_map.source_x;
      int hint_y = s_field_map.source_y;
      uint8_t hint_mask = 0;
      if (physical_x != s_field_map.physical_x) {
        hint_x += delta_x >= 0 ? 16 : -16;
        if (hint_x >= s_field_map.bound_x0 &&
            hint_x <= (int)s_field_map.bound_x1 - 16)
          hint_mask |= 1u;
      } else {
        hint_mask |= 1u;
      }
      if (physical_y != s_field_map.physical_y) {
        hint_y += delta_y >= 0 ? 16 : -16;
        if (hint_y >= 0 && hint_y <= kSmrpgMapSubtiles - 16)
          hint_mask |= 2u;
      } else {
        hint_mask |= 2u;
      }
      s_field_map.have_hint = hint_mask != 0;
      s_field_map.hint_mask = hint_mask;
      if (hint_mask & 1u) s_field_map.hint_x = (uint8_t)hint_x;
      if (hint_mask & 2u) s_field_map.hint_y = (uint8_t)hint_y;
      s_field_map.valid = false;
  }
  if (!s_field_map.valid && !smrpg_find_field_map()) return false;

  s_field_map.have_scroll = true;
  s_field_map.last_hscroll =
      (uint16_t)g_ppu->hScroll[0] & 0x03ffu;
  s_field_map.last_vscroll =
      (uint16_t)g_ppu->vScroll[0] & 0x03ffu;

  for (int layer = 0; layer < 2; layer++) {
    const uint32_t hscroll = (uint16_t)g_ppu->hScroll[layer] & 0x03ffu;
    const uint32_t vscroll = (uint16_t)g_ppu->vScroll[layer] & 0x03ffu;
    const uint32_t camera_tile_x =
        (uint32_t)s_field_map.source_x +
        (physical_tile_x - (unsigned)s_field_map.physical_x);
    const uint32_t camera_tile_y =
        (uint32_t)s_field_map.source_y +
        (physical_tile_y - (unsigned)s_field_map.physical_y);
    const uint32_t world_x =
        kSmrpgShadowBasePixel +
        camera_tile_x * 8u + (hscroll & 7u);
    const uint32_t world_y =
        kSmrpgShadowBasePixel +
        camera_tile_y * 8u + (vscroll & 7u);
    WsShadowSetWorld(layer, world_x, world_y);
    WsShadowSetScroll(layer, hscroll, vscroll);
    WsShadowSetBlankTile(layer, 0x0100);
  }
  return true;
}

static void smrpg_fill_field_shadow(void) {
  if (!s_field_map.valid) return;

  static uint8_t occupied[kSmrpgMapSubtiles * kSmrpgMapSubtiles];
  static uint8_t edge_connected[kSmrpgMapSubtiles * kSmrpgMapSubtiles];

  for (int layer = 0; layer < 2; layer++) {
    const unsigned physical_tile_y =
        ((uint16_t)g_ppu->vScroll[layer] >> 3) & 31u;
    const unsigned source_y0 =
        (unsigned)s_field_map.source_y +
        (physical_tile_y - (unsigned)s_field_map.physical_y);
    const unsigned source_y1 =
        source_y0 + 30u < kSmrpgMapSubtiles
            ? source_y0 + 30u
            : kSmrpgMapSubtiles - 1u;
    const unsigned physical_tile_x =
        ((uint16_t)g_ppu->hScroll[layer] >> 3) & 31u;
    const unsigned camera_x =
        (unsigned)s_field_map.source_x +
        (physical_tile_x - (unsigned)s_field_map.physical_x);

    if (layer == 0) {
      memset(occupied, 0, sizeof(occupied));
      memset(edge_connected, 0, sizeof(edge_connected));
      for (unsigned y = source_y0; y <= source_y1; y++) {
        for (unsigned x = s_field_map.bound_x0;
             x < s_field_map.bound_x1; x++) {
          occupied[y * kSmrpgMapSubtiles + x] =
              smrpg_is_signal_tile(smrpg_full_map_tile(0, x, y)) ||
              smrpg_is_signal_tile(smrpg_full_map_tile(1, x, y));
        }
      }

      for (unsigned y = source_y0; y <= source_y1; y++) {
        for (int side = 0; side < 2; side++) {
          const int edge_x =
              side == 0 ? (int)camera_x : (int)camera_x + 31;
          for (int distance = 1; distance <= 10; distance++) {
            const int x =
                side == 0 ? edge_x - distance : edge_x + distance;
            if (x < s_field_map.bound_x0 ||
                x >= s_field_map.bound_x1)
              break;
            bool column_continues = false;
            for (int dy = -1; dy <= 1; dy++) {
              const int neighbor_y = (int)y + dy;
              if (neighbor_y < (int)source_y0 ||
                  neighbor_y > (int)source_y1)
                continue;
              const unsigned index =
                  (unsigned)neighbor_y * kSmrpgMapSubtiles +
                  (unsigned)x;
              if (occupied[index]) {
                column_continues = true;
                break;
              }
            }
            if (!column_continues)
              break;
            const unsigned index =
                y * kSmrpgMapSubtiles + (unsigned)x;
            if (occupied[index])
              edge_connected[index] = 1;
          }
        }
      }
    }

    for (unsigned source_y = source_y0;
         source_y <= source_y1; source_y++) {
      const uint32_t key_y =
          kSmrpgShadowBaseTile + source_y;
      for (unsigned source_x = 0;
           source_x < kSmrpgMapSubtiles; source_x++) {
        const uint32_t key_x =
            kSmrpgShadowBaseTile + source_x;
        /*
         * The first margin chunk may share the final native-view tile when
         * fine scroll is nonzero. Preserve the entire 33-tile viewport span
         * before applying component filtering farther into the margins;
         * otherwise the host boundary inserts one blank 8-pixel seam.
         */
        const bool overlaps_native =
            source_x >= camera_x && source_x <= camera_x + 32u;
        WsShadowForceTile(
            layer, key_x, key_y,
            source_x >= s_field_map.bound_x0 &&
                    source_x < s_field_map.bound_x1 &&
                    (overlaps_native ||
                     edge_connected[
                         source_y * kSmrpgMapSubtiles + source_x])
                ? smrpg_full_map_tile(layer, source_x, source_y)
                : 0x0100u);
      }
    }
  }
}

static void smrpg_field_side_space(int *left_out, int *right_out) {
  const unsigned physical_tile_x =
      ((uint16_t)g_ppu->hScroll[0] >> 3) & 31u;
  const uint32_t camera_tile_x =
      (uint32_t)s_field_map.source_x +
      (physical_tile_x - (unsigned)s_field_map.physical_x);
  const int camera_x =
      (int)(camera_tile_x * 8u +
            ((uint16_t)g_ppu->hScroll[0] & 7u));
  *left_out = camera_x - (int)s_field_map.bound_x0 * 8;
  *right_out =
      (int)s_field_map.bound_x1 * 8 - (camera_x + 256);
}

static uint16_t read_vector(uint16_t address) {
  uint8_t low = cpu_read8(&g_cpu, 0, address);
  uint8_t high = cpu_read8(&g_cpu, 0, (uint16_t)(address + 1u));
  return (uint16_t)(low | ((uint16_t)high << 8));
}

static uint16_t nmi_vector_address(void) {
  return g_cpu.emulation ? 0xfffau : 0xffeau;
}

static uint16_t irq_vector_address(void) {
  return g_cpu.emulation ? 0xfffeu : 0xffeeu;
}

static bool irq_pending(void) {
  Sa1 *sa1 = g_snes->cart->sa1;
  return g_snes->inIrq || (sa1 && sa1_cpu_irq_pending(sa1));
}

static int run_interrupt(uint16_t vector_address, uint64_t deadline) {
  cpu_push_interrupt_frame_at(&g_cpu, s_resume_pc);
  interp_bridge_set_master_deadline(deadline);
  int result =
      interp_bridge_run_interrupt(&g_cpu, read_vector(vector_address));
  interp_bridge_set_master_deadline(0);
  return result;
}

static void synchronize_hardware(void) {
  snes_sync_master_clock(g_snes, g_cpu.master_cycles);
  cart_sync_coprocessors(g_snes->cart, g_cpu.master_cycles);
}

static bool run_main_slice(uint64_t deadline) {
  uint64_t before = g_cpu.master_cycles;
  interp_bridge_set_master_deadline(deadline);
  s_last_lle_result =
      interp_bridge_run_until_quiescent(&g_cpu, s_resume_pc);
  interp_bridge_set_master_deadline(0);
  uint32_t resume = interp_bridge_lle_resume_pc();
  if (resume) s_resume_pc = resume;
  synchronize_hardware();
  return s_last_lle_result && g_cpu.master_cycles != before;
}

static void run_one_frame(void) {
  if (!s_initialized) {
    cpu_state_init(&g_cpu, g_ram);
    s_resume_pc = read_vector(0xfffcu);
    /* The first interrupt edge is scanline 225, not the end of scanline 261.
     * Keeping every recurring deadline on that beam phase matters for games
     * that perform work only while $4212 reports vblank. */
    s_next_frame_master = kFirstVblankMaster;
    s_host_frames = 0;
    s_last_lle_result = 1;
    s_initialized = true;
    fprintf(stderr, "[smrpg] boot RESET=$%06x SA-1=%s\n",
            (unsigned)s_resume_pc,
            cart_has_sa1(g_snes->cart) ? "enabled" : "missing");
  }

  while (s_next_frame_master <= g_cpu.master_cycles)
    s_next_frame_master += kMasterClocksPerFrame;
  const uint64_t deadline = s_next_frame_master;
  unsigned slices = 0;
  unsigned interrupts = 0;

  if (s_host_frames > 0 && g_snes->nmiEnabled) {
    g_snes->inNmi = true;
    s_last_lle_result = run_interrupt(nmi_vector_address(), deadline);
    g_snes->inNmi = false;
  }

  while (s_last_lle_result && g_cpu.master_cycles < deadline &&
         slices++ < kMaximumSlicesPerFrame) {
    bool progressed = run_main_slice(deadline);
    bool took_wai = interp_bridge_lle_took_wai() != 0;

    if (irq_pending() && !g_cpu._flag_I) {
      if (interrupts++ >= kMaximumInterruptsPerFrame) {
        fprintf(stderr,
                "[smrpg] IRQ storm at frame=%u resume=$%06x\n",
                s_host_frames, (unsigned)s_resume_pc);
        s_last_lle_result = 0;
        break;
      }
      s_last_lle_result = run_interrupt(irq_vector_address(), deadline);
      synchronize_hardware();
      continue;
    }

    if (g_cpu.master_cycles >= deadline) break;
    if (took_wai) {
      /* WAI resumes only when an interrupt edge occurs. Reconstructing the
       * bridge one scanline later loses Interp816::waiting, so park the host
       * CPU explicitly while the beam and SA-1 continue to run. */
      bool woke_for_irq = false;
      while (g_cpu.master_cycles < deadline) {
        uint64_t remaining = deadline - g_cpu.master_cycles;
        uint32_t idle =
            remaining > kMasterClocksPerLine
                ? kMasterClocksPerLine
                : (uint32_t)remaining;
        g_cpu.master_cycles += idle;
        synchronize_hardware();
        if (irq_pending() && !g_cpu._flag_I) {
          if (interrupts++ >= kMaximumInterruptsPerFrame) {
            s_last_lle_result = 0;
            break;
          }
          s_last_lle_result =
              run_interrupt(irq_vector_address(), deadline);
          synchronize_hardware();
          woke_for_irq = s_last_lle_result != 0;
          break;
        }
      }
      if (!woke_for_irq) break;
      continue;
    }
    if (!progressed) {
      uint64_t remaining = deadline - g_cpu.master_cycles;
      uint32_t idle =
          remaining > kMasterClocksPerLine
              ? kMasterClocksPerLine
              : (uint32_t)remaining;
      g_cpu.master_cycles += idle;
      synchronize_hardware();
    }
  }

  if (slices >= kMaximumSlicesPerFrame) {
    fprintf(stderr,
            "[smrpg] slice cap at frame=%u resume=$%06x master=%llu\n",
            s_host_frames, (unsigned)s_resume_pc,
            (unsigned long long)g_cpu.master_cycles);
    s_last_lle_result = 0;
  }

  if (g_cpu.master_cycles < deadline) {
    g_cpu.master_cycles = deadline;
    synchronize_hardware();
  }

  s_frame_hdmaen = g_snesrecomp_last_hdmaen;
  s_next_frame_master += kMasterClocksPerFrame;
  s_host_frames++;

  if (s_host_frames <= 16 || (s_host_frames % 600u) == 0) {
    Sa1 *sa1 = g_snes->cart->sa1;
    fprintf(stderr,
            "[smrpg] frame=%u resume=$%06x P=%02x E=%u master=%llu "
            "sa1_insn=%llu slices=%u irq=%u\n",
            s_host_frames, (unsigned)s_resume_pc, g_cpu.P,
            g_cpu.emulation, (unsigned long long)g_cpu.master_cycles,
            (unsigned long long)(sa1 ? sa1_instructions_executed(sa1) : 0),
            slices, interrupts);
  }
}

void SmrpgBeginDrawing(uint8_t *pixels, size_t pitch) {
  int render_flags = kPpuRenderFlags_NewRenderer;
  if (g_ws_active) render_flags |= kPpuRenderFlags_NoSpriteLimits;
  PpuBeginDrawing(g_ppu, pixels, pitch, render_flags);
}

void SmrpgSetWidescreenExtra(int extra) {
  if (extra < 0) extra = 0;
  if (extra > kWsExtraMax) extra = kWsExtraMax;
  g_ws_extra = extra;
  g_ws_active = extra > 0;
}

void SmrpgSetWidescreenHud(bool enabled) { s_widescreen_hud = enabled; }

int SmrpgWidescreenWidth(void) { return 256 + g_ws_extra * 2; }

static void configure_widescreen_policy(void) {
  if (!g_ws_active) {
    PpuSetExtraSpace(g_ppu, 0);
    PpuWsSetOamRightHints(g_ppu, NULL);
    return;
  }

  /*
   * Field presentation is identified from the PPU contract observed through
   * the TCP scanline debugger, rather than an inferred game WRAM variable:
   * Mode 1, BG1/BG2/OBJ on the main screen, the same layers windowed, and
   * inverted Window 1 selected for BG1/BG2/OBJ plus the color window. SMRPG's
   * visible field lines then drive W1 to 8..247 with HDMA.
   *
   * This signature is deliberately presentation-only. Menus and bounded
   * screens stay centered until their own signatures have been validated.
   */
  const bool field_play =
      (g_ppu->bgmode & 7u) == 1 &&
      g_ppu->screenEnabled[0] == 0x13 &&
      g_ppu->screenWindowed[0] == 0x13 &&
      g_ppu->windowsel == 0x00330333u;
  const bool battle_play =
      (g_ppu->bgmode & 7u) == 1 &&
      (g_ppu->screenEnabled[0] & 0x11u) == 0x11u &&
      g_ppu->screenWindowed[0] == 0 &&
      g_ppu->windowsel == 0 &&
      g_ppu->bgXsc[0] == 0x41u;
  const bool field_shadow =
      field_play && smrpg_prepare_field_shadow();
  PpuSetExtraSpaceCentered(g_ppu, (uint8_t)g_ws_extra);
  if (field_shadow) {
    int extra_left;
    int extra_right;
    smrpg_field_side_space(&extra_left, &extra_right);
    PpuSetExtraSideSpace(g_ppu, extra_left, extra_right, 0);
  }
  PpuSetWidescreenLayerClamp(g_ppu, 1u << 2);
  if (field_shadow) {
    /*
     * The hardware-authentic inverted W1 admits only 8..247. Extend that
     * game-authored field window into the host side margins for BG1, BG2,
     * OBJ, and color composition. This changes no emulated PPU register.
     */
    PpuSetWidescreenWindowExpansion(
        g_ppu, (1u << 0) | (1u << 1) | (1u << 4) | (1u << 5), 1u);
  } else {
    PpuSetWidescreenWindowExpansion(g_ppu, 0, 0);
  }
  {
    /*
     * SMRPG has not yet rewritten its CPU-side OAM staging into the new
     * right-margin coordinate range. Treat ambiguous 9-bit X values as the
     * authentic negative-wrap sprites; otherwise native offscreen actors in
     * $100..$15F are mistaken for widescreen-right actors and corrupt scenes.
     */
    static const uint8_t no_right_margin_hints[16] = {0};
    PpuWsSetOamRightHints(g_ppu, no_right_margin_hints);
  }

  /*
   * TCP OAM snapshots identify battle slots 0..3 as the command diamond and
   * slots 4..7 as Mario's portrait/HP. Battle actors begin at slot 8. Anchor
   * exactly those eight HUD slots to the adaptive edge; the arena and every
   * actor stay in authentic world coordinates.
   */
  if (battle_play && s_widescreen_hud) {
    PpuSetWidescreenLayerAnchorBand(g_ppu, 1, 0, 40, 144, 192);
    PpuSetWsHudOamBand(g_ppu, 224, 112, 160);
    PpuSetWsHudOamShiftRange(g_ppu, 0, 8);
    PpuSetWidescreenHudAlwaysVisible(g_ppu, true);
  } else {
    PpuSetWsHudOamBand(g_ppu, 0, 0, 0);
    PpuSetWsHudOamShiftRange(g_ppu, 0, 0);
    PpuSetWidescreenHudAlwaysVisible(g_ppu, false);
  }
}

void SmrpgDrawPpuFrame(void) {
  SimpleHdma channels[8];
  bool active[8] = {false};

  configure_widescreen_policy();
  WsShadowFrame(g_ppu);
  smrpg_fill_field_shadow();
  dma_startDma(g_dma, s_frame_hdmaen, true);
  for (int channel = 0; channel < 8; channel++) {
    SimpleHdma_Init(&channels[channel], &g_dma->channel[channel]);
    active[channel] = (s_frame_hdmaen & (1u << channel)) != 0;
  }
  for (int line = 0; line <= 224; line++) {
    for (int channel = 0; channel < 8; channel++) {
      if (active[channel]) SimpleHdma_DoLine(&channels[channel]);
    }
    ppu_runLine(g_ppu, line);
  }
}

static void session_reset(void) {
  s_initialized = false;
  s_resume_pc = 0;
  s_next_frame_master = 0;
  s_host_frames = 0;
  s_last_lle_result = 1;
  s_frame_hdmaen = 0;
  s_loaded_runtime_state = false;
  memset(&s_field_map, 0, sizeof(s_field_map));
  WsShadowReset();
  interp_bridge_set_master_deadline(0);
}

enum {
  kSmrpgStateMagic = 0x47525053u, /* "SPRG" */
  kSmrpgStateVersion = 1u,
};

typedef struct SmrpgRuntimeState {
  uint32_t magic;
  uint32_t version;
  CpuState cpu;
  uint32_t resume_pc;
  uint64_t next_frame_master;
  uint32_t host_frames;
  int32_t last_lle_result;
  uint8_t frame_hdmaen;
  uint8_t initialized;
  uint8_t memsel;
  uint8_t last_hdmaen;
  uint8_t reserved[4];
  int32_t snes_frame;
  uint64_t main_cpu_cycles_estimate;
  uint64_t apu_pace_cycles_estimate;
} SmrpgRuntimeState;

static void smrpg_state_save_extra(SaveLoadInfo *sli) {
  SmrpgRuntimeState state;
  memset(&state, 0, sizeof(state));
  state.magic = kSmrpgStateMagic;
  state.version = kSmrpgStateVersion;
  state.cpu = g_cpu;
  /* Never persist an address-space-dependent host pointer. */
  state.cpu.ram = NULL;
  state.resume_pc = s_resume_pc;
  state.next_frame_master = s_next_frame_master;
  state.host_frames = s_host_frames;
  state.last_lle_result = s_last_lle_result;
  state.frame_hdmaen = s_frame_hdmaen;
  state.initialized = s_initialized;
  state.memsel = g_memsel;
  state.last_hdmaen = g_snesrecomp_last_hdmaen;
  state.snes_frame = snes_frame_counter;
  state.main_cpu_cycles_estimate = g_main_cpu_cycles_estimate;
  state.apu_pace_cycles_estimate = g_apu_pace_cycles_estimate;
  sli->func(sli, &state, sizeof(state));
}

static void smrpg_state_load_extra(SaveLoadInfo *sli, uint32_t version) {
  SmrpgRuntimeState state;
  (void)version;
  memset(&state, 0, sizeof(state));
  sli->func(sli, &state, sizeof(state));
  s_loaded_runtime_state =
      state.magic == kSmrpgStateMagic &&
      state.version == kSmrpgStateVersion;
  if (!s_loaded_runtime_state) return;

  g_cpu = state.cpu;
  g_cpu.ram = g_ram;
  s_resume_pc = state.resume_pc;
  s_next_frame_master = state.next_frame_master;
  s_host_frames = state.host_frames;
  s_last_lle_result = state.last_lle_result;
  s_frame_hdmaen = state.frame_hdmaen;
  s_initialized = state.initialized != 0;
  g_memsel = state.memsel;
  g_snesrecomp_last_hdmaen = state.last_hdmaen;
  snes_frame_counter = state.snes_frame;
  g_main_cpu_cycles_estimate = state.main_cpu_cycles_estimate;
  g_apu_pace_cycles_estimate = state.apu_pace_cycles_estimate;
}

static void smrpg_on_state_loaded(uint32_t version) {
  (void)version;
  if (!s_loaded_runtime_state) return;

  /*
   * These are host pacing cursors, not guest state. Point them at the restored
   * counters so the first APU access cannot underflow against the future state
   * that existed immediately before Load was pressed.
   */
  g_apu_last_sync_master = g_cpu.master_cycles;
  g_apu_last_sync_cycles = g_apu_pace_cycles_estimate;
  g_snes->beamMasterLast = g_cpu.master_cycles;
  interp_bridge_set_master_deadline(0);
  s_loaded_runtime_state = false;
}

static const RtlGameInfo kSmrpgGameInfo = {
    .title = "super_mario_rpg",
    .initialize = NULL,
    .run_frame = run_one_frame,
    .draw_ppu_frame = SmrpgDrawPpuFrame,
    .save_name_prefix = "smrpg",
    .state_save_extra = smrpg_state_save_extra,
    .state_load_extra = smrpg_state_load_extra,
    .on_state_loaded = smrpg_on_state_loaded,
    .session_reset = session_reset,
};

const RtlGameInfo *SmrpgGameInfo(void) { return &kSmrpgGameInfo; }
uint32_t SmrpgResumePc(void) { return s_resume_pc; }
int SmrpgLastLleResult(void) { return s_last_lle_result; }
