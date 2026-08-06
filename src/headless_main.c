#include "smrpg_runtime.h"

#include "audio_trace.h"
#include "common_rtl.h"
#include "cpu_state.h"
#include "sha256.h"
#include "snes/apu.h"
#include "snes/cart.h"
#include "snes/dsp.h"
#include "snes/ppu.h"
#include "snes/sa1.h"
#include "snes/snes.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t kSmrpgSha256[32] = {
    0x74, 0x06, 0x46, 0xf3, 0x53, 0x5b, 0xfb, 0x36,
    0x5c, 0xa4, 0x4e, 0x70, 0xd4, 0x6a, 0xb4, 0x33,
    0x46, 0x7b, 0x14, 0x2b, 0xd8, 0x40, 0x10, 0x39,
    0x30, 0x70, 0xbd, 0x0b, 0x14, 0x1a, 0xf8, 0x53,
};

typedef struct AttractStats {
  uint64_t logic_hash;
  uint64_t video_hash;
  uint64_t logic_changes;
  uint64_t video_changes;
  uint64_t video_active_frames;
  uint64_t audio_active_frames;
  uint32_t audio_peak;
  uint64_t audio_underruns;
} AttractStats;

typedef struct WavWriter {
  FILE *stream;
  uint32_t data_bytes;
} WavWriter;

typedef struct InputSpan {
  long first;
  long last;
  uint32_t mask;
} InputSpan;

enum { kMaxInputSpans = 128 };

void headless_install_exception_filter(void);

static uint64_t fnv1a_update(uint64_t hash, const void *data, size_t size) {
  const uint8_t *bytes = (const uint8_t *)data;
  for (size_t i = 0; i < size; i++) {
    hash ^= bytes[i];
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

static uint64_t logic_hash(void) {
  uint64_t hash = UINT64_C(14695981039346656037);
  hash = fnv1a_update(hash, g_ram, sizeof(g_ram));
  if (g_snes->cart->ram && g_snes->cart->ramSize)
    hash = fnv1a_update(hash, g_snes->cart->ram,
                        g_snes->cart->ramSize);
  return hash;
}

static uint8_t *read_rom(const char *path, size_t *size_out,
                         uint8_t hash[32]) {
  FILE *stream = fopen(path, "rb");
  if (!stream) return NULL;
  if (fseek(stream, 0, SEEK_END) != 0) {
    fclose(stream);
    return NULL;
  }
  long length = ftell(stream);
  if (length <= 0 || fseek(stream, 0, SEEK_SET) != 0) {
    fclose(stream);
    return NULL;
  }
  uint8_t *rom = (uint8_t *)malloc((size_t)length);
  if (!rom || fread(rom, 1, (size_t)length, stream) != (size_t)length) {
    free(rom);
    fclose(stream);
    return NULL;
  }
  fclose(stream);
  size_t skip = (size_t)length % 1024u == 512u ? 512u : 0u;
  *size_out = (size_t)length - skip;
  if (skip) memmove(rom, rom + skip, *size_out);
  sha256_compute(rom, *size_out, hash);
  return rom;
}

static void print_hash(const uint8_t hash[32]) {
  for (unsigned i = 0; i < 32; i++) fprintf(stderr, "%02x", hash[i]);
}

static void write_u16(FILE *stream, uint16_t value) {
  fputc(value & 0xff, stream);
  fputc(value >> 8, stream);
}

static void write_u32(FILE *stream, uint32_t value) {
  write_u16(stream, value & 0xffff);
  write_u16(stream, value >> 16);
}

static int wav_open(WavWriter *writer, const char *path) {
  memset(writer, 0, sizeof(*writer));
  if (!path || !path[0]) return 1;
  writer->stream = fopen(path, "wb");
  if (!writer->stream) return 0;
  fwrite("RIFF", 1, 4, writer->stream);
  write_u32(writer->stream, 0);
  fwrite("WAVEfmt ", 1, 8, writer->stream);
  write_u32(writer->stream, 16);
  write_u16(writer->stream, 1);
  write_u16(writer->stream, 2);
  write_u32(writer->stream, 32040);
  write_u32(writer->stream, 32040 * 4);
  write_u16(writer->stream, 4);
  write_u16(writer->stream, 16);
  fwrite("data", 1, 4, writer->stream);
  write_u32(writer->stream, 0);
  return ferror(writer->stream) == 0;
}

static int wav_append(WavWriter *writer, const int16_t *audio, int frames) {
  if (!writer->stream) return 1;
  size_t bytes = (size_t)frames * 2u * sizeof(audio[0]);
  if (writer->data_bytes > UINT32_MAX - bytes) return 0;
  if (fwrite(audio, 1, bytes, writer->stream) != bytes) return 0;
  writer->data_bytes += (uint32_t)bytes;
  return 1;
}

static int wav_close(WavWriter *writer) {
  if (!writer->stream) return 1;
  fseek(writer->stream, 4, SEEK_SET);
  write_u32(writer->stream, writer->data_bytes + 36u);
  fseek(writer->stream, 40, SEEK_SET);
  write_u32(writer->stream, writer->data_bytes);
  int ok = ferror(writer->stream) == 0 && fclose(writer->stream) == 0;
  writer->stream = NULL;
  return ok;
}

static int write_ppm(const char *path, const uint8_t *pixels, int width) {
  if (!path || !path[0]) return 1;
  FILE *stream = fopen(path, "wb");
  if (!stream) return 0;
  fprintf(stream, "P6\n%d 224\n255\n", width);
  for (size_t i = 0; i < (size_t)width * 224u; i++) {
    uint8_t rgb[3] = {pixels[i * 4u + 2u], pixels[i * 4u + 1u],
                      pixels[i * 4u]};
    if (fwrite(rgb, 1, sizeof(rgb), stream) != sizeof(rgb)) {
      fclose(stream);
      return 0;
    }
  }
  return fclose(stream) == 0;
}

static int write_wram_dump(const char *path) {
  if (!path || !path[0]) return 1;
  FILE *stream = fopen(path, "wb");
  if (!stream) return 0;
  int ok = fwrite(g_ram, 1, sizeof(g_ram), stream) == sizeof(g_ram);
  if (fclose(stream) != 0) ok = 0;
  return ok;
}

static int maybe_write_raw_frame(long frame, const uint8_t *pixels,
                                 int width) {
  static int initialized;
  static const char *directory;
  static long from;
  static long to;
  static long step;
  if (!initialized) {
    initialized = 1;
    directory = getenv("SNESRECOMP_FRAME_DUMP_DIR");
    const char *value = getenv("SNESRECOMP_FRAME_DUMP_FROM");
    from = value && value[0] ? strtol(value, NULL, 10) : 0;
    value = getenv("SNESRECOMP_FRAME_DUMP_TO");
    to = value && value[0] ? strtol(value, NULL, 10) : -1;
    value = getenv("SNESRECOMP_FRAME_DUMP_STEP");
    step = value && value[0] ? strtol(value, NULL, 10) : 1;
    if (step < 1) step = 1;
  }
  if (!directory || !directory[0] || frame < from ||
      (to >= 0 && frame > to) || (frame - from) % step)
    return 1;
  char path[1024];
  if (snprintf(path, sizeof(path), "%s/frame_%06ld.raw", directory,
               frame) >= (int)sizeof(path))
    return 0;
  FILE *stream = fopen(path, "wb");
  if (!stream) return 0;
  size_t size = (size_t)width * 224u * 4u;
  int ok = fwrite(pixels, 1, size, stream) == size;
  if (fclose(stream) != 0) ok = 0;
  return ok;
}

static int trace_wram(uint64_t frame) {
  static int initialized;
  static FILE *stream;
  static uint8_t previous[0x2000];
  static int primed;
  if (!initialized) {
    initialized = 1;
    const char *path = getenv("SNESRECOMP_WRAM_TRACE_FILE");
    if (path && path[0]) stream = fopen(path, "w");
    if (path && path[0] && !stream) return 0;
  }
  if (!stream) return 1;
  for (uint32_t address = 0; address < sizeof(previous); address++) {
    uint8_t value = g_ram[address];
    if (!primed || value != previous[address]) {
      fprintf(stream,
              "{\"f\":%llu,\"adr\":\"0x%05x\",\"old\":\"0x%02x\","
              "\"val\":\"0x%02x\"}\n",
              (unsigned long long)frame, address,
              primed ? previous[address] : 0, value);
      previous[address] = value;
    }
  }
  primed = 1;
  if (frame % 30u == 0) fflush(stream);
  return ferror(stream) == 0;
}

static void collect_video(AttractStats *stats, const uint8_t *pixels,
                          long frame, int width) {
  uint64_t hash = fnv1a_update(UINT64_C(14695981039346656037), pixels,
                               (size_t)width * 224u * 4u);
  if (frame && hash != stats->video_hash) stats->video_changes++;
  stats->video_hash = hash;
  const uint32_t *words = (const uint32_t *)pixels;
  uint32_t first = words[0] & 0xffffffu;
  for (size_t i = 1; i < (size_t)width * 224u; i++) {
    if ((words[i] & 0xffffffu) != first) {
      stats->video_active_frames++;
      break;
    }
  }
}

static void collect_audio(AttractStats *stats, const int16_t *audio,
                          int frames) {
  int active = 0;
  for (int i = 0; i < frames * 2; i++) {
    int64_t value = audio[i];
    uint32_t magnitude = (uint32_t)(value < 0 ? -value : value);
    if (magnitude) active = 1;
    if (magnitude > stats->audio_peak) stats->audio_peak = magnitude;
  }
  if (active) stats->audio_active_frames++;
}

static int parse_input_script(InputSpan spans[kMaxInputSpans],
                              size_t *count_out) {
  const char *cursor = getenv("SNESRECOMP_INPUT_SCRIPT");
  *count_out = 0;
  if (!cursor || !cursor[0]) return 1;
  while (*cursor) {
    while (*cursor == ' ' || *cursor == '\t' || *cursor == ',') cursor++;
    if (!*cursor) break;
    if (*count_out >= kMaxInputSpans) return 0;
    char *end = NULL;
    long first = strtol(cursor, &end, 0);
    if (end == cursor || first < 0) return 0;
    cursor = end;
    long last = first;
    if (*cursor == '-') {
      cursor++;
      last = strtol(cursor, &end, 0);
      if (end == cursor || last < first) return 0;
      cursor = end;
    }
    if (*cursor++ != ':') return 0;
    unsigned long mask = strtoul(cursor, &end, 0);
    if (end == cursor || mask > 0x0fffu) return 0;
    cursor = end;
    while (*cursor == ' ' || *cursor == '\t') cursor++;
    if (*cursor && *cursor != ',') return 0;
    spans[*count_out].first = first;
    spans[*count_out].last = last;
    spans[*count_out].mask = (uint32_t)mask;
    (*count_out)++;
  }
  return 1;
}

static uint32_t scripted_input(const InputSpan *spans, size_t count,
                               long frame) {
  uint32_t input = 0;
  for (size_t i = 0; i < count; i++) {
    if (frame >= spans[i].first && frame <= spans[i].last)
      input |= spans[i].mask;
  }
  return input;
}

int main(int argc, char **argv) {
  /* Preserve the last guest-frame diagnostic if a soak terminates abnormally
   * while stderr is redirected to a qualification log. */
  setvbuf(stderr, NULL, _IONBF, 0);
  headless_install_exception_filter();
  if (argc < 2 || argc > 3) {
    fprintf(stderr,
            "usage: SuperMarioRPGSNESRecompHeadless <smrpg.sfc> [frames]\n");
    return 2;
  }
  long frame_limit = argc == 3 ? strtol(argv[2], NULL, 10) : 36000;
  if (frame_limit < 1 || frame_limit > 1000000) {
    fprintf(stderr, "frames must be between 1 and 1000000\n");
    return 2;
  }

  size_t rom_size = 0;
  uint8_t rom_hash[32];
  uint8_t *rom = read_rom(argv[1], &rom_size, rom_hash);
  if (!rom) {
    fprintf(stderr, "unable to read ROM: %s\n", argv[1]);
    return 2;
  }
  if (rom_size != 0x400000u ||
      memcmp(rom_hash, kSmrpgSha256, sizeof(rom_hash)) != 0) {
    fprintf(stderr, "unsupported Super Mario RPG ROM (size=%zu sha256=",
            rom_size);
    print_hash(rom_hash);
    fputs(")\n", stderr);
    free(rom);
    return 2;
  }

  RtlRegisterGame(SmrpgGameInfo());
  if (!SnesInit(rom, (int)rom_size) || !cart_has_sa1(g_snes->cart)) {
    fputs("failed to initialize the SA-1 cartridge\n", stderr);
    free(rom);
    return 3;
  }
  const char *save_root = getenv("SNESRECOMP_SAVE_ROOT");
  if (save_root && save_root[0]) RtlSetSaveRoot(save_root);
  RtlReadSram();

  InputSpan input_spans[kMaxInputSpans];
  size_t input_span_count = 0;
  if (!parse_input_script(input_spans, &input_span_count)) {
    fputs("invalid SNESRECOMP_INPUT_SCRIPT; expected "
          "FIRST[-LAST]:MASK entries\n", stderr);
    free(rom);
    return 2;
  }

  int widescreen_extra = 0;
  const char *wide_value = getenv("SNESRECOMP_WIDESCREEN_EXTRA");
  if (wide_value && wide_value[0])
    widescreen_extra = (int)strtol(wide_value, NULL, 0);
  SmrpgSetWidescreenExtra(widescreen_extra);
  const char *hud_value = getenv("SNESRECOMP_WIDESCREEN_HUD");
  if (hud_value && hud_value[0])
    SmrpgSetWidescreenHud(strtol(hud_value, NULL, 0) != 0);
  int frame_width = SmrpgWidescreenWidth();

  static uint8_t pixels[kPpuBufWidth * 224u * 4u];
  int16_t audio[534 * 2];
  SmrpgBeginDrawing(pixels, (size_t)frame_width * 4u);
  AttractStats stats = {0};
  WavWriter wav;
  if (!wav_open(&wav, getenv("SNESRECOMP_WAV"))) {
    fputs("unable to open WAV capture\n", stderr);
    free(rom);
    return 4;
  }
  double audio_accumulator = 0.0;
  long save_state_frame = -1;
  long load_state_frame = -1;
  const char *state_value = getenv("SNESRECOMP_SAVE_STATE_FRAME");
  if (state_value && state_value[0])
    save_state_frame = strtol(state_value, NULL, 0);
  state_value = getenv("SNESRECOMP_LOAD_STATE_FRAME");
  if (state_value && state_value[0])
    load_state_frame = strtol(state_value, NULL, 0);

  for (long frame = 0; frame < frame_limit; frame++) {
    (void)RtlRunFrame(scripted_input(input_spans, input_span_count, frame));
    if (g_fail || !SmrpgLastLleResult()) {
      fprintf(stderr, "smrpg_native: runtime failure frame=%ld pc=$%06x\n",
              frame, (unsigned)SmrpgResumePc());
      wav_close(&wav);
      free(rom);
      return 5;
    }
    if (frame == save_state_frame) RtlSaveLoad(kSaveLoad_Save, 0);
    if (frame == load_state_frame) RtlSaveLoad(kSaveLoad_Load, 0);

    uint64_t next_logic = logic_hash();
    if (frame && next_logic != stats.logic_hash) stats.logic_changes++;
    stats.logic_hash = next_logic;
    if (!trace_wram((uint64_t)frame)) {
      fputs("unable to write WRAM trace\n", stderr);
      wav_close(&wav);
      free(rom);
      return 6;
    }

    SmrpgDrawPpuFrame();
    collect_video(&stats, pixels, frame, frame_width);
    if (!maybe_write_raw_frame(frame, pixels, frame_width)) {
      fputs("unable to write raw frame capture\n", stderr);
      wav_close(&wav);
      free(rom);
      return 6;
    }

    audio_accumulator += 32040.0 / 60.098811862;
    int audio_frames = (int)audio_accumulator;
    audio_accumulator -= audio_frames;
    memset(audio, 0, sizeof(audio));
    RtlRenderAudio(audio, audio_frames, 2);
    collect_audio(&stats, audio, audio_frames);
    if (!wav_append(&wav, audio, audio_frames)) {
      fputs("unable to write WAV capture\n", stderr);
      wav_close(&wav);
      free(rom);
      return 7;
    }
  }

  int output_ok = wav_close(&wav) &&
                  write_ppm(getenv("SNESRECOMP_FRAME_DUMP"), pixels,
                            frame_width) &&
                  write_wram_dump(getenv("SNESRECOMP_WRAM_DUMP"));
  Sa1 *sa1 = g_snes->cart->sa1;
  uint64_t sa1_instructions =
      sa1 ? sa1_instructions_executed(sa1) : 0;
  uint32_t audio_samples = g_snes->apu->dsp->sampleWrite;
  /* Authoritative underrun count. This soak previously tracked no shortfall
   * accounting at all here; rtl_render_native (common_rtl.c) already counts
   * every real delivery shortfall against its true threshold (frames * ratio
   * + 2 native samples, not a naive `available < audio_frames`) via
   * audio_trace_on_output_underflow, so read that counter directly rather
   * than recompute the threshold in this host. */
  AudioTraceStats audio_stats;
  audio_trace_get_stats(&audio_stats);
  stats.audio_underruns = audio_stats.output_underflows;
  int qualified =
      frame_limit < 600 ||
      (sa1_instructions > 0 &&
       stats.logic_changes >= (uint64_t)(frame_limit / 20) &&
       stats.video_active_frames >= (uint64_t)(frame_limit / 4) &&
       stats.video_changes >= (uint64_t)(frame_limit / 600) &&
       stats.audio_active_frames >= (uint64_t)(frame_limit / 10) &&
       stats.audio_peak > 0 && audio_samples > 0 &&
       stats.audio_underruns == 0);

  fprintf(stderr,
          "smrpg_native: %s frames=%ld resume=%06x master=%llu "
          "sa1_instructions=%llu logic_changes=%llu "
          "video_active=%llu video_changes=%llu audio_samples=%u "
          "audio_active=%llu audio_peak=%u audio_underruns=%llu\n",
          qualified && output_ok ? "PASS" : "FAIL", frame_limit,
          (unsigned)SmrpgResumePc(),
          (unsigned long long)g_cpu.master_cycles,
          (unsigned long long)sa1_instructions,
          (unsigned long long)stats.logic_changes,
          (unsigned long long)stats.video_active_frames,
          (unsigned long long)stats.video_changes, audio_samples,
          (unsigned long long)stats.audio_active_frames, stats.audio_peak,
          (unsigned long long)stats.audio_underruns);
  free(rom);
  return qualified && output_ok ? 0 : 8;
}
