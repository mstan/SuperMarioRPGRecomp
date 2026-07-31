#include "smrpg_runtime.h"

#include "common_rtl.h"
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

#include <SDL.h>

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

bool g_new_ppu = true;
static SDL_mutex *g_audio_mutex;

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

void host_report_init(const char *game_name, const char *build_version) {
  (void)game_name;
  (void)build_version;
}
void host_report_breadcrumb(const char *format, ...) { (void)format; }
void host_report_fatal(const char *message) {
  if (message) fprintf(stderr, "fatal: %s\n", message);
}
int host_report_has_fatal(void) { return 0; }
void host_report_dump_json(FILE *stream) { (void)stream; }
const char *host_report_write_minidump(void *info) {
  (void)info;
  return NULL;
}
const char *host_report_preserve_crash_copy(const char *path) {
  (void)path;
  return NULL;
}
void host_report_crash_test_tick(void) {}

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
  const Uint8 *keys = SDL_GetKeyboardState(NULL);
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

static void SDLCALL audio_callback(void *userdata, Uint8 *stream, int len) {
  (void)userdata;
  if (!g_snes || len < 4) {
    SDL_memset(stream, 0, (size_t)len);
    return;
  }
  RtlRenderAudio((int16_t *)stream, len / 4, 2);
}

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
    SDL_GetRendererOutputSize(renderer, &width, &height);
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

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
    fprintf(stderr, "SDL initialization failed: %s\n", SDL_GetError());
    free(rom);
    return 3;
  }
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,
              launcher_settings.linear_filter ? "linear" : "nearest");
  g_audio_mutex = SDL_CreateMutex();
  if (!g_audio_mutex) Die("Unable to create the audio mutex");

  RtlRegisterGame(SmrpgGameInfo());
  if (!SnesInit(rom, (int)rom_size) || !cart_has_sa1(g_snes->cart))
    Die("SNESRecomp rejected the SA-1 cartridge");
  RtlReadSram();

  SDL_Window *window =
      SDL_CreateWindow("Super Mario RPG: Legend of the Seven Stars",
                       SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                       launcher_settings.adaptive_view ? 960 : 768,
                       launcher_settings.adaptive_view ? 540 : 576,
                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  if (!window) Die("Unable to create the game window");
  if (launcher_settings.fullscreen)
    SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
  SDL_Renderer *renderer =
      SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
  if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
  if (!renderer) Die("Unable to create the game renderer");
  SDL_Texture *texture =
      SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                        SDL_TEXTUREACCESS_STREAMING,
                        kPpuBufWidth, kFrameHeight);
  if (!texture) Die("Unable to create the game texture");

  static uint8_t pixels[kPpuBufWidth * kFrameHeight * kBytesPerPixel];
  SmrpgSetWidescreenExtra(0);
  SmrpgSetWidescreenHud(launcher_settings.widescreen_hud != 0);
  SmrpgBeginDrawing(pixels, kFrameWidth * kBytesPerPixel);

  SDL_AudioSpec wanted = {0};
  SDL_AudioSpec obtained = {0};
  wanted.freq = 32040;
  wanted.format = AUDIO_S16SYS;
  wanted.channels = 2;
  wanted.samples = 1024;
  wanted.callback = audio_callback;
  SDL_AudioDeviceID audio =
      SDL_OpenAudioDevice(NULL, 0, &wanted, &obtained, 0);
  if (!audio) Die("Unable to open the audio device");
  SDL_PauseAudioDevice(audio, launcher_settings.enable_audio ? 0 : 1);

  SDL_GameController *pad = NULL;
  for (int i = 0; i < SDL_NumJoysticks(); i++) {
    if (SDL_IsGameController(i)) {
      pad = SDL_GameControllerOpen(i);
      if (pad) break;
    }
  }

  int running = 1;
  int paused = 0;
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
        switch (event.key.keysym.sym) {
          case SDLK_ESCAPE:
            running = 0;
            break;
          case SDLK_p:
            paused = !paused;
            SDL_PauseAudioDevice(audio, paused);
            break;
          case SDLK_F5:
            RtlSaveLoad(kSaveLoad_Save, 0);
            break;
          case SDLK_F9:
            RtlSaveLoad(kSaveLoad_Load, 0);
            break;
          case SDLK_F11: {
            Uint32 flags = SDL_GetWindowFlags(window);
            SDL_SetWindowFullscreen(
                window, (flags & SDL_WINDOW_FULLSCREEN_DESKTOP)
                            ? 0
                            : SDL_WINDOW_FULLSCREEN_DESKTOP);
            break;
          }
          default:
            break;
        }
      }
    }

    logical_width = update_adaptive_widescreen(
        renderer, launcher_settings.adaptive_view, pixels);
    if (!paused) {
      uint32_t input =
          keyboard_input() | controller_input(pad) | (1u << 30);
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
    SDL_RenderCopy(renderer, texture, &source, NULL);
    SDL_RenderPresent(renderer);
    pace_frame(&next_frame_counter, frame_counters);
    if (auto_close_frames > 0 && frames >= auto_close_frames) running = 0;
  }

  const char *frame_dump = getenv("SNESRECOMP_FRAME_BMP");
  if (!write_frame_bmp(frame_dump, pixels, logical_width, kFrameHeight))
    fprintf(stderr, "Unable to write frame dump: %s\n", frame_dump);
  RtlWriteSram();
  SDL_PauseAudioDevice(audio, 1);
  SDL_CloseAudioDevice(audio);
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
