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
#include "smrpg_renderer.h"
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
static int s_presentation_extra;
static bool s_custom_renderer_enabled;
static uint8_t *s_output;
static size_t s_output_pitch;
static uint32_t s_native_frame[256 * 224];

static void observe_field_drawing(void *context, const Sa1Observation *o) {
  (void)context;
  if (o->pc == 0xc0aaf3)
    SmrpgRendererLatchActor(o->bwram, o->bwram_size, o->iram, o->x);
  else if (o->pc == 0xc06e5f)
    SmrpgRendererSubmitActors(o->bwram, o->bwram_size);
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

  if (s_custom_renderer_enabled || getenv("SMRPG_RENDER_CAPTURE") ||
      getenv("SMRPG_RENDER_CAPTURE_DIR")) {
    Cart *cart = g_snes->cart;
    SmrpgRendererSetRom(cart->rom, cart->romSize);
    static const uint32_t addresses[] = {0xc0aaf3, 0xc06e5f};
    sa1_set_observer(cart->sa1, addresses, 2, observe_field_drawing, NULL);
  } else {
    sa1_set_observer(g_snes->cart->sa1, NULL, 0, NULL, NULL);
  }

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

#if SNESRECOMP_TRACE
  /* Boot/heartbeat progress trace: trace builds only. */
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
#else
  (void)slices;
  (void)interrupts;
#endif
}

void SmrpgBeginDrawing(uint8_t *pixels, size_t pitch) {
  s_output = pixels;
  s_output_pitch = pitch;
  PpuSetExtraSpace(g_ppu, 0);
  PpuBeginDrawing(g_ppu, (uint8_t *)s_native_frame, 256 * 4,
                  kPpuRenderFlags_NewRenderer);
}

void SmrpgSetWidescreenExtra(int extra) {
  if (extra < 0) extra = 0;
  if (extra > (kSmrpgRenderWidth - 256) / 2)
    extra = (kSmrpgRenderWidth - 256) / 2;
  s_presentation_extra = extra;
  g_ws_active = false;
  g_ws_extra = 0;
}

void SmrpgSetCustomRendererEnabled(bool enabled) { s_custom_renderer_enabled = enabled; }
int SmrpgWidescreenWidth(void) { return 256 + s_presentation_extra * 2; }

void SmrpgDrawPpuFrame(void) {
  SimpleHdma channels[8];
  bool active[8] = {false};

  const char *capture_path = getenv("SMRPG_RENDER_CAPTURE");
  const char *capture_dir = getenv("SMRPG_RENDER_CAPTURE_DIR");
  bool capture = s_custom_renderer_enabled || (capture_path && *capture_path) ||
                 (capture_dir && *capture_dir);
  if (capture) {
    Cart *cart = g_snes->cart;
    SmrpgRendererBeginFrame(g_ram, cart->ram, cart->ramSize,
        sa1_cpu_memory_ptr(cart->sa1, 0, 0x3000));
    SmrpgRendererSelectActors(g_ppu);
  }
  dma_startDma(g_dma, s_frame_hdmaen, true);
  for (int channel = 0; channel < 8; channel++) {
    SimpleHdma_Init(&channels[channel], &g_dma->channel[channel]);
    active[channel] = (s_frame_hdmaen & (1u << channel)) != 0;
  }
  for (int line = 0; line <= 224; line++) {
    for (int channel = 0; channel < 8; channel++) {
      if (active[channel]) SimpleHdma_DoLine(&channels[channel]);
    }
    if (capture && line > 0) SmrpgRendererCaptureLine(g_ppu, line - 1);
    ppu_runLine(g_ppu, line);
  }
  if (capture) {
    SmrpgRendererEndFrame(s_native_frame);
    const char *at = getenv("SMRPG_RENDER_CAPTURE_FRAME");
    if (capture_path && at && s_host_frames == (unsigned)strtoul(at, NULL, 0))
      SmrpgRendererSaveCapture(capture_path);
    const char *interval = getenv("SMRPG_RENDER_CAPTURE_INTERVAL");
    unsigned period = interval ? (unsigned)strtoul(interval, NULL, 0) : 600;
    if (capture_dir && *capture_dir && period && s_host_frames % period == 0) {
      char path[1024];
      snprintf(path, sizeof(path), "%s/frame-%06u.srpg", capture_dir, s_host_frames);
      if (!SmrpgRendererSaveCapture(path)) fprintf(stderr, "Unable to write renderer capture: %s\n", path);
    }
  }
  if (s_output && s_custom_renderer_enabled) {
    SmrpgRendererDraw(s_output, s_output_pitch, SmrpgWidescreenWidth());
  } else if (s_output) {
    /* Diagnostic captures do not opt stock rendering into the compositor. */
    for (unsigned y = 0; y < 224; ++y)
      memcpy(s_output + y * s_output_pitch, s_native_frame + y * 256, 256 * 4);
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
  SmrpgRendererReset();
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
  SmrpgRendererReset();
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
