#include "smrpg_runtime.h"

#include "common_rtl.h"
#include "cpu_trace.h"
#include "debug_server.h"
#include "host_report.h"
#include "launcher_profile.h"
#include "recomp_launcher.h"
#include "sha256.h"
#include "snes/cart.h"
#include "snes/ppu.h"
#include "snes/snes.h"
#include "spc_player.h"
#include "types.h"
#include "widescreen.h"

#include "desktop/sdl_compat.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  kFrameWidth = 256,
  kFrameHeight = 224,
  kBytesPerPixel = 4,
};

static const uint8_t kSmrpgSha256[32] = {
    0x74, 0x06, 0x46, 0xf3, 0x53, 0x5b, 0xfb, 0x36,
    0x5c, 0xa4, 0x4e, 0x70, 0xd4, 0x6a, 0xb4, 0x33,
    0x46, 0x7b, 0x14, 0x2b, 0xd8, 0x40, 0x10, 0x39,
    0x30, 0x70, 0xbd, 0x0b, 0x14, 0x1a, 0xf8, 0x53,
};
static const char *const kSmrpgSha1[] = {
    "a4f7539054c359fe3f360b0e6b72e394439fe9df",
};

/* Baked in by -DSNESRECOMP_BUILD_VERSION=<ver> (see CMakeLists.txt); local
 * builds report "dev". tools/make_release.ps1 verifies the stamp by scanning
 * the packaged binary for this literal, so it must reach .rodata. */
#ifndef SNESRECOMP_BUILD_VERSION
#define SNESRECOMP_BUILD_VERSION "dev"
#endif
static const char kBuildVersion[] = SNESRECOMP_BUILD_VERSION;

bool g_new_ppu = true;
static SDL_mutex *g_audio_mutex;
static const char kWindowTitle[] =
    "Super Mario RPG: Legend of the Seven Stars";

static void spc_initialize(SpcPlayer *player) { (void)player; }
static void spc_upload(SpcPlayer *player, const uint8_t *data) {
  (void)player;
  (void)data;
}

static SpcPlayer g_sdl_spc_player = {
    .initialize = spc_initialize,
    .upload = spc_upload,
};

SpcPlayer *g_spc_player = &g_sdl_spc_player;

void NORETURN Die(const char *error) {
  fprintf(stderr, "fatal: %s\n", error ? error : "unknown error");
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Super Mario RPG",
                           error ? error : "Unknown error", NULL);
  exit(EXIT_FAILURE);
}

void RtlApuLock(void) {
  if (g_audio_mutex) SDL_LockMutex(g_audio_mutex);
}

void RtlApuUnlock(void) {
  if (g_audio_mutex) SDL_UnlockMutex(g_audio_mutex);
}

/* host_report_* now comes from the engine's runner/src/host_report.c (see
 * CMakeLists.txt) rather than local no-op stubs, so SMRPG gets the same
 * breadcrumb ring, minidump capture and version stamping as the other games. */

static uint8_t *read_rom(const char *path, size_t *size_out) {
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
  return rom;
}

static int verify_rom(const uint8_t *rom, size_t size) {
  uint8_t actual[32];
  if (size != 0x400000u) return 0;
  sha256_compute(rom, size, actual);
  return memcmp(actual, kSmrpgSha256, sizeof(actual)) == 0;
}

static int resolve_rom(int argc, char **argv, char *path, size_t path_size,
                       RecompLauncherCSettings *settings) {
  memset(settings, 0, sizeof(*settings));
  settings->window_scale = 3;
  settings->enable_audio = 1;
  settings->audio_freq = 32040;
  settings->volume = 100;
  settings->player_src[0] = 1;
  settings->deadzone[0] = 25;
  settings->adaptive_view = 1;
  settings->widescreen_hud = 1;
  {
    /* Match the established SNESRecomp diagnostic override. Adaptive 16:9 is
     * the game default; the launcher or environment can select authentic
     * native-width presentation without changing guest state. */
    const char *widescreen = getenv("SNESRECOMP_WIDESCREEN");
    if (widescreen && widescreen[0]) {
      settings->adaptive_view =
          strcmp(widescreen, "Adaptive") == 0 ||
          strcmp(widescreen, "adaptive") == 0 ||
          strtol(widescreen, NULL, 0) != 0;
    }
    const char *hud = getenv("SNESRECOMP_WIDESCREEN_HUD");
    if (hud && hud[0]) settings->widescreen_hud = strtol(hud, NULL, 0) != 0;
  }

  if (argc > 1) {
    snprintf(path, path_size, "%s", argv[1]);
    return 1;
  }

  RecompLauncherCGameInfo game;
  memset(&game, 0, sizeof(game));
  launcher_profile_apply("snes", &game);
  game.name = "Super Mario RPG: Legend of the Seven Stars";
  game.region = "(USA)";
  game.known_sha1_hex = kSmrpgSha1;
  game.num_known_sha1 = 1;
  game.known_sha256 = &kSmrpgSha256;
  game.num_known_sha256 = 1;
  game.num_players = 1;
  game.sram_path = "saves/smrpg.srm";
  game.widescreen_supported = 0;
  game.adaptive_view_supported = 1;
  game.aspect_experimental = 1;
  game.rom_cache_path = "rom.cfg";

  char initial_rom[1024] = {0};
  char assets_dir[1024] = ".";
  if (argv[0] && argv[0][0]) {
    snprintf(assets_dir, sizeof(assets_dir), "%s", argv[0]);
    char *slash = strrchr(assets_dir, '/');
    char *backslash = strrchr(assets_dir, '\\');
    char *separator = slash > backslash ? slash : backslash;
    if (separator)
      *separator = '\0';
    else
      snprintf(assets_dir, sizeof(assets_dir), "%s", ".");
  }
  FILE *probe = fopen("smrpg.sfc", "rb");
  if (probe) {
    fclose(probe);
    snprintf(initial_rom, sizeof(initial_rom), "%s", "smrpg.sfc");
  }

  int action = recomp_launcher_run_window(
      "Super Mario RPG \xE2\x80\x94 Launcher", settings, &game, assets_dir,
      initial_rom, path, path_size);
  if (action == 1) return 0;
  if (action == 0 && path[0]) return 1;
  if (initial_rom[0]) {
    snprintf(path, path_size, "%s", initial_rom);
    return 1;
  }
  return -1;
}

static uint32_t keyboard_input(void) {
  /* SDL3 returns const bool*, SDL2 const Uint8*; the shim normalizes both. */
  const uint8_t *keys = snesrecomp_sdl_get_keyboard_state();
  uint32_t input = 0;
  if (keys[SDL_SCANCODE_Z]) input |= 0x0001u;
  if (keys[SDL_SCANCODE_A]) input |= 0x0002u;
  if (keys[SDL_SCANCODE_RSHIFT]) input |= 0x0004u;
  if (keys[SDL_SCANCODE_RETURN]) input |= 0x0008u;
  if (keys[SDL_SCANCODE_UP]) input |= 0x0010u;
  if (keys[SDL_SCANCODE_DOWN]) input |= 0x0020u;
  if (keys[SDL_SCANCODE_LEFT]) input |= 0x0040u;
  if (keys[SDL_SCANCODE_RIGHT]) input |= 0x0080u;
  if (keys[SDL_SCANCODE_X]) input |= 0x0100u;
  if (keys[SDL_SCANCODE_S]) input |= 0x0200u;
  if (keys[SDL_SCANCODE_Q]) input |= 0x0400u;
  if (keys[SDL_SCANCODE_W]) input |= 0x0800u;
  return input;
}

static uint32_t controller_input(SDL_GameController *pad) {
  if (!pad) return 0;
  uint32_t input = 0;
  if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A))
    input |= 0x0001u;
  if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_X))
    input |= 0x0002u;
  if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK))
    input |= 0x0004u;
  if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START))
    input |= 0x0008u;
  if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP))
    input |= 0x0010u;
  if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN))
    input |= 0x0020u;
  if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT))
    input |= 0x0040u;
  if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT))
    input |= 0x0080u;
  if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B))
    input |= 0x0100u;
  if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_Y))
    input |= 0x0200u;
  if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER))
    input |= 0x0400u;
  if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER))
    input |= 0x0800u;

  Sint16 x = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
  Sint16 y = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
  if (x < -12000) input |= 0x0040u;
  if (x > 12000) input |= 0x0080u;
  if (y < -12000) input |= 0x0010u;
  if (y > 12000) input |= 0x0020u;
  return input;
}

static void fill_audio(Uint8 *stream, int len) {
  if (!g_snes || len < 4) {
    SDL_memset(stream, 0, (size_t)len);
    return;
  }
  RtlRenderAudio((int16_t *)stream, len / 4, 2);
}

#if SNESRECOMP_SDL3
/* SDL3 replaced the pull callback with an SDL_AudioStream the app pushes into.
 * The stream sizes each pull itself, so render into a scratch buffer grown on
 * demand and hand the whole block over. */
static SDL_AudioStream *g_audio_stream;
static Uint8 *g_audio_scratch;
static size_t g_audio_scratch_size;

static void SDLCALL audio_stream_callback(void *userdata,
                                          SDL_AudioStream *stream,
                                          int additional_amount,
                                          int total_amount) {
  (void)userdata;
  (void)total_amount;
  if (additional_amount <= 0) return;

  /* Keep a cushion queued rather than feeding exactly what was asked for.
   *
   * SDL2's pull callback wrote straight into a fixed 1024-frame device buffer,
   * so the emulator always had ~32 ms of slack to produce the next block. SDL3
   * pulls from the stream in whatever chunk the backend wants (measured: ~320
   * frames / 10 ms, varying call to call, resampling 32040 Hz S16 up to a
   * 44100 Hz F32 device), and if we only ever push `additional_amount` the
   * stream holds no reserve at all - any hitch in the APU underruns
   * immediately and is audible as static.
   *
   * Top the queue back up to the historical 1024-frame depth. Surplus is not
   * lost: the stream keeps it for the next pull. */
  const int frame_bytes = 2 * (int)sizeof(int16_t);   /* stereo S16 */
  const int target_queued = 1024 * frame_bytes;       /* 32 ms @ 32040 Hz */
  int queued = SDL_GetAudioStreamQueued(stream);
  if (queued < 0) queued = 0;
  int want_bytes = additional_amount;
  if (target_queued - queued > want_bytes) want_bytes = target_queued - queued;
  /* The docs warn byte counts "might be slightly overestimated"; nothing
   * promises frame alignment, and a partial frame would permanently swap L/R. */
  want_bytes = ((want_bytes + frame_bytes - 1) / frame_bytes) * frame_bytes;

  if ((size_t)want_bytes > g_audio_scratch_size) {
    Uint8 *resized = (Uint8 *)realloc(g_audio_scratch, (size_t)want_bytes);
    if (!resized) return;
    g_audio_scratch = resized;
    g_audio_scratch_size = (size_t)want_bytes;
  }
  fill_audio(g_audio_scratch, want_bytes);
  SDL_PutAudioStreamData(stream, g_audio_scratch, want_bytes);
}
#else
static void SDLCALL audio_callback(void *userdata, Uint8 *stream, int len) {
  (void)userdata;
  fill_audio(stream, len);
}
#endif

static void pace_frame(double *next_counter, double frame_counters) {
  *next_counter += frame_counters;
  double now = (double)SDL_GetPerformanceCounter();
  if (now + frame_counters * 4.0 < *next_counter ||
      now > *next_counter + frame_counters * 4.0) {
    *next_counter = now;
    return;
  }
  double frequency = (double)SDL_GetPerformanceFrequency();
  while (now < *next_counter) {
    double remaining_ms = (*next_counter - now) * 1000.0 / frequency;
    if (remaining_ms > 1.5)
      SDL_Delay((Uint32)(remaining_ms - 0.5));
    else
      SDL_Delay(0);
    now = (double)SDL_GetPerformanceCounter();
  }
}

static int write_frame_bmp(const char *path, const uint8_t *pixels,
                           int width, int height) {
  if (!path || !path[0]) return 1;
  FILE *stream = fopen(path, "wb");
  if (!stream) return 0;
  uint32_t image_size = (uint32_t)width * (uint32_t)height * 4u;
  uint32_t pixel_offset = 54;
  uint32_t file_size = pixel_offset + image_size;
  uint8_t header[54] = {'B', 'M'};
  uint32_t dib_size = 40;
  int32_t bmp_width = width;
  int32_t bmp_height = -height;
  uint16_t planes = 1;
  uint16_t bits_per_pixel = 32;
  memcpy(header + 2, &file_size, 4);
  memcpy(header + 10, &pixel_offset, 4);
  memcpy(header + 14, &dib_size, 4);
  memcpy(header + 18, &bmp_width, 4);
  memcpy(header + 22, &bmp_height, 4);
  memcpy(header + 26, &planes, 2);
  memcpy(header + 28, &bits_per_pixel, 2);
  memcpy(header + 34, &image_size, 4);
  int ok = fwrite(header, 1, sizeof(header), stream) == sizeof(header) &&
           fwrite(pixels, 1, image_size, stream) == image_size;
  if (fclose(stream) != 0) ok = 0;
  return ok;
}

/* Uint64 deadlines: SDL3's SDL_GetTicks returns Uint64 and SDL_TICKS_PASSED is
 * gone, so compare absolute deadlines directly. Widening is harmless under
 * SDL2, where SDL_GetTicks returns Uint32. */
static void set_state_feedback(SDL_Window *window, const char *operation,
                               int slot, int ok, Uint64 *until) {
  char title[192];
  snprintf(title, sizeof(title), "%s - State %s %s (slot %d)",
           kWindowTitle, operation, ok ? "succeeded" : "failed", slot + 1);
  SDL_SetWindowTitle(window, title);
  *until = (Uint64)SDL_GetTicks() + 2500u;
}

static void perform_state_action(SDL_Window *window, int save,
                                 int slot, Uint64 *feedback_until) {
  char path[128];
  if (save) RtlEnsureSaveDir();
  RtlSaveSlotPath(slot, path, sizeof(path));
  set_state_feedback(window, save ? "save" : "load", slot,
                     save ? RtlSaveSnapshot(path) : RtlLoadSnapshot(path),
                     feedback_until);
}

static void update_state_feedback(SDL_Window *window, Uint64 *feedback_until) {
  if (*feedback_until && (Uint64)SDL_GetTicks() >= *feedback_until) {
    SDL_SetWindowTitle(window, kWindowTitle);
    *feedback_until = 0;
  }
}

static int adaptive_extra_for_size(int drawable_width, int drawable_height) {
  if (drawable_width <= 0 || drawable_height <= 0) return 0;
  int64_t numerator = (int64_t)drawable_width * kFrameHeight -
                      (int64_t)drawable_height * kFrameWidth;
  if (numerator <= 0) return 0;
  int64_t divisor = (int64_t)drawable_height * 2;
  int64_t extra = (numerator + divisor / 2) / divisor;
  return extra > kPpuExtraLeftRight ? kPpuExtraLeftRight : (int)extra;
}

static int update_adaptive_widescreen(SDL_Renderer *renderer,
                                      int adaptive_view, uint8_t *pixels) {
  int extra = 0;
  if (adaptive_view) {
    int width = 0;
    int height = 0;
    /* Must be the TRUE physical output size; the shim routes SDL3 to
     * SDL_GetRenderOutputSize, not the logical-presentation-adjusted
     * "Current" variant that silently pinned Zelda to 4:3. */
    snesrecomp_sdl_get_render_output_size(renderer, &width, &height);
    extra = adaptive_extra_for_size(width, height);
  }
  if (extra != g_ws_extra) {
    SmrpgSetWidescreenExtra(extra);
    SmrpgBeginDrawing(pixels, (size_t)SmrpgWidescreenWidth() * kBytesPerPixel);
  }
  return SmrpgWidescreenWidth();
}

int main(int argc, char **argv) {
  SDL_SetMainReady();
  host_report_init(kWindowTitle, kBuildVersion);
  char rom_path[1024] = {0};
  RecompLauncherCSettings launcher_settings;
  int resolve_result =
      resolve_rom(argc, argv, rom_path, sizeof(rom_path), &launcher_settings);
  if (resolve_result <= 0) return resolve_result == 0 ? 0 : 2;
  size_t rom_size = 0;
  uint8_t *rom = read_rom(rom_path, &rom_size);
  if (!rom || !verify_rom(rom, rom_size)) {
    fprintf(stderr, "A verified Super Mario RPG (USA) ROM is required: %s\n",
            rom_path);
    free(rom);
    return 2;
  }

  /* SDL_Init flipped to true-on-success in SDL3 and inverts silently in its
   * old `!= 0` form, so it must route through the shim. */
  if (!snesrecomp_sdl_init(SDL_INIT_VIDEO | SDL_INIT_AUDIO |
                           SDL_INIT_GAMECONTROLLER)) {
    fprintf(stderr, "SDL initialization failed: %s\n", SDL_GetError());
    free(rom);
    return 3;
  }
  g_audio_mutex = SDL_CreateMutex();
  if (!g_audio_mutex) Die("Unable to create the audio mutex");

  RtlRegisterGame(SmrpgGameInfo());
  if (!SnesInit(rom, (int)rom_size) || !cart_has_sa1(g_snes->cart))
    Die("SNESRecomp rejected the SA-1 cartridge");
  cpu_trace_init();
  debug_server_set_ram(g_snes->ram, 0x20000);
  {
    int debug_port = 4381;
    const char *port_value = getenv("SNESRECOMP_DEBUG_PORT");
    if (port_value && port_value[0]) {
      long parsed = strtol(port_value, NULL, 0);
      if (parsed > 0 && parsed <= 65535) debug_port = (int)parsed;
    }
    if (debug_server_init(debug_port) == 0) {
#if SNESRECOMP_TRACE
      fprintf(stderr, "[smrpg] Debug server ready on port %d\n", debug_port);
#endif
    }
  }
  RtlReadSram();

  /* SDL_WINDOW_ALLOW_HIGHDPI is one of the few old names SDL3 does NOT alias
   * in SDL_oldnames.h; it became SDL_WINDOW_HIGH_PIXEL_DENSITY. */
#if SNESRECOMP_SDL3
  const SDL_WindowFlags kHighDpiFlag = SDL_WINDOW_HIGH_PIXEL_DENSITY;
#else
  const Uint32 kHighDpiFlag = SDL_WINDOW_ALLOW_HIGHDPI;
#endif
  SDL_Window *window = snesrecomp_sdl_create_window(
      kWindowTitle,
      launcher_settings.adaptive_view ? 960 : 768,
      launcher_settings.adaptive_view ? 540 : 576,
      SDL_WINDOW_RESIZABLE | kHighDpiFlag);
  if (!window) Die("Unable to create the game window");
  if (launcher_settings.fullscreen)
    snesrecomp_sdl_set_fullscreen(window, true);
  SDL_Renderer *renderer = snesrecomp_sdl_create_renderer(window, false, false);
  if (!renderer) renderer = snesrecomp_sdl_create_renderer(window, true, false);
  if (!renderer) Die("Unable to create the game renderer");
  SDL_Texture *texture =
      SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                        SDL_TEXTUREACCESS_STREAMING,
                        kPpuBufWidth, kFrameHeight);
  if (!texture) Die("Unable to create the game texture");
  /* Scale quality is per-texture in SDL3 (the SDL2 render hint is gone), and
   * the SNES framebuffer leaves alpha zero, so it must be marked opaque or
   * SDL3 blends the whole frame away and presents only the clear color. */
  snesrecomp_sdl_set_texture_linear(texture,
                                    launcher_settings.linear_filter != 0);
  snesrecomp_sdl_set_texture_opaque(texture);

  static uint8_t pixels[kPpuBufWidth * kFrameHeight * kBytesPerPixel];
  SmrpgSetWidescreenExtra(0);
  SmrpgSetWidescreenHud(launcher_settings.widescreen_hud != 0);
  SmrpgBeginDrawing(pixels, kFrameWidth * kBytesPerPixel);

  SDL_AudioSpec wanted = {0};
  wanted.freq = 32040;
  /* Native rate, so the conversion is a no-op — but state it rather than
   * leaning on the consumer's default. */
  RtlSetAudioOutputRate(32040);
  wanted.format = AUDIO_S16SYS;
  wanted.channels = 2;
  SDL_AudioDeviceID audio = 0;
#if SNESRECOMP_SDL3
  /* SDL_AudioSpec has no `samples`/`callback` in SDL3: the device is opened as
   * a stream and the callback is supplied separately. */
  g_audio_stream = SDL_OpenAudioDeviceStream(
      SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &wanted, audio_stream_callback, NULL);
  if (g_audio_stream) audio = SDL_GetAudioStreamDevice(g_audio_stream);
#else
  SDL_AudioSpec obtained = {0};
  wanted.samples = 1024;
  wanted.callback = audio_callback;
  audio = SDL_OpenAudioDevice(NULL, 0, &wanted, &obtained, 0);
#endif
  if (!audio) Die("Unable to open the audio device");
  snesrecomp_sdl_pause_audio_device(audio, launcher_settings.enable_audio == 0);

  SDL_GameController *pad = NULL;
#if SNESRECOMP_SDL3
  {
    /* SDL3 enumerates by instance ID rather than by index. */
    int njs = 0;
    SDL_JoystickID *joysticks = SDL_GetJoysticks(&njs);
    for (int i = 0; i < njs; i++) {
      if (!SDL_IsGamepad(joysticks[i])) continue;
      pad = SDL_OpenGamepad(joysticks[i]);
      if (pad) break;
    }
    SDL_free(joysticks);
  }
#else
  for (int i = 0; i < SDL_NumJoysticks(); i++) {
    if (SDL_IsGameController(i)) {
      pad = SDL_GameControllerOpen(i);
      if (pad) break;
    }
  }
#endif

  int running = 1;
  int paused = 0;
  Uint64 state_feedback_until = 0;
  long frames = 0;
  long auto_close_frames = 0;
  const char *auto_close = getenv("SNESRECOMP_AUTOCLOSE_FRAMES");
  if (auto_close) auto_close_frames = strtol(auto_close, NULL, 10);
  int logical_width = kFrameWidth;
  double frame_counters =
      (double)SDL_GetPerformanceFrequency() / 60.098811862;
  double next_frame_counter = (double)SDL_GetPerformanceCounter();
  while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT) running = 0;
      if (event.type == SDL_KEYDOWN && !event.key.repeat) {
        /* The keysym struct was flattened in SDL3; the shim macros pick the
         * right member for each major. */
        const SDL_Keycode key = SNESRECOMP_SDL_EVENT_KEY(event);
        const Uint16 mod = (Uint16)SNESRECOMP_SDL_EVENT_MOD(event);
        if (key >= SDLK_F1 && key <= SDLK_F12) {
          int slot = (int)(key - SDLK_F1);
          perform_state_action(window, (mod & KMOD_SHIFT) != 0, slot,
                               &state_feedback_until);
          continue;
        }
        switch (key) {
          case SDLK_ESCAPE:
            running = 0;
            break;
          case SDLK_p:
            paused = !paused;
            snesrecomp_sdl_pause_audio_device(audio, paused != 0);
            break;
          case SDLK_RETURN: {
            if (!(mod & KMOD_ALT)) break;
            Uint32 flags = (Uint32)SDL_GetWindowFlags(window);
            snesrecomp_sdl_set_fullscreen(
                window,
                (flags & SNESRECOMP_SDL_WINDOW_FULLSCREEN_DESKTOP) == 0);
            break;
          }
          default:
            break;
        }
      }
    }

    update_state_feedback(window, &state_feedback_until);
    {
      int slot = debug_server_consume_loadstate();
      if (slot >= 0) perform_state_action(
          window, 0, slot, &state_feedback_until);
      slot = debug_server_consume_savestate();
      if (slot >= 0) perform_state_action(
          window, 1, slot, &state_feedback_until);
    }
    debug_server_wait_if_paused();
    logical_width = update_adaptive_widescreen(
        renderer, launcher_settings.adaptive_view, pixels);
    if (!paused) {
      uint32_t input =
          keyboard_input() | controller_input(pad) |
          debug_server_get_controller_inputs() |
          (1u << 30) | debug_server_get_controller_active_mask();
      (void)RtlRunFrame(input);
      if (g_fail || !SmrpgLastLleResult())
        Die("Super Mario RPG runtime execution failed");
      SmrpgDrawPpuFrame();
      SDL_Rect update_rect = {0, 0, logical_width, kFrameHeight};
      SDL_UpdateTexture(texture, &update_rect, pixels,
                        logical_width * kBytesPerPixel);
      frames++;
    }

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_Rect source = {0, 0, logical_width, kFrameHeight};
    snesrecomp_sdl_render_texture(renderer, texture, &source, NULL);
    SDL_RenderPresent(renderer);
    pace_frame(&next_frame_counter, frame_counters);
    if (auto_close_frames > 0 && frames >= auto_close_frames) running = 0;
  }

  const char *frame_dump = getenv("SNESRECOMP_FRAME_BMP");
  if (!write_frame_bmp(frame_dump, pixels, logical_width, kFrameHeight))
    fprintf(stderr, "Unable to write frame dump: %s\n", frame_dump);
  RtlWriteSram();
  debug_server_shutdown();
  snesrecomp_sdl_pause_audio_device(audio, true);
#if SNESRECOMP_SDL3
  /* Destroying the stream closes the device it was opened against. */
  SDL_DestroyAudioStream(g_audio_stream);
  g_audio_stream = NULL;
  free(g_audio_scratch);
  g_audio_scratch = NULL;
  g_audio_scratch_size = 0;
#else
  SDL_CloseAudioDevice(audio);
#endif
  if (pad) SDL_GameControllerClose(pad);
  SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_DestroyMutex(g_audio_mutex);
  g_audio_mutex = NULL;
  SDL_Quit();
  free(rom);
  return 0;
}
