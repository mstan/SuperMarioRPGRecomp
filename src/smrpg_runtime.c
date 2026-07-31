#include "smrpg_runtime.h"

#include "common_rtl.h"
#include "cpu_state.h"
#include "snes/cart.h"
#include "snes/dma.h"
#include "snes/interp_bridge.h"
#include "snes/ppu.h"
#include "snes/sa1.h"
#include "snes/snes.h"

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
  PpuBeginDrawing(g_ppu, pixels, pitch, 0);
}

void SmrpgDrawPpuFrame(void) {
  SimpleHdma channels[8];
  bool active[8] = {false};
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
  interp_bridge_set_master_deadline(0);
}

static const RtlGameInfo kSmrpgGameInfo = {
    .title = "super_mario_rpg",
    .initialize = NULL,
    .run_frame = run_one_frame,
    .draw_ppu_frame = SmrpgDrawPpuFrame,
    .save_name_prefix = "smrpg",
    .state_save_extra = NULL,
    .state_load_extra = NULL,
    .on_state_loaded = NULL,
    .session_reset = session_reset,
};

const RtlGameInfo *SmrpgGameInfo(void) { return &kSmrpgGameInfo; }
uint32_t SmrpgResumePc(void) { return s_resume_pc; }
int SmrpgLastLleResult(void) { return s_last_lle_result; }
