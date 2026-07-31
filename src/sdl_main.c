#include "smrpg_runtime.h"

#include "common_rtl.h"
#include "host_report.h"
#include "sha256.h"
#include "snes/cart.h"
#include "snes/snes.h"
#include "spc_player.h"
#include "types.h"

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

int main(int argc, char **argv) {
  SDL_SetMainReady();
  const char *rom_path = argc > 1 ? argv[1] : "smrpg.sfc";
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
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
  g_audio_mutex = SDL_CreateMutex();
  if (!g_audio_mutex) Die("Unable to create the audio mutex");

  RtlRegisterGame(SmrpgGameInfo());
  if (!SnesInit(rom, (int)rom_size) || !cart_has_sa1(g_snes->cart))
    Die("SNESRecomp rejected the SA-1 cartridge");
  RtlReadSram();

  SDL_Window *window =
      SDL_CreateWindow("Super Mario RPG: Legend of the Seven Stars",
                       SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                       768, 576,
                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  if (!window) Die("Unable to create the game window");
  SDL_Renderer *renderer =
      SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
  if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
  if (!renderer) Die("Unable to create the game renderer");
  SDL_Texture *texture =
      SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                        SDL_TEXTUREACCESS_STREAMING,
                        kFrameWidth, kFrameHeight);
  if (!texture) Die("Unable to create the game texture");

  static uint8_t pixels[kFrameWidth * kFrameHeight * kBytesPerPixel];
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
  SDL_PauseAudioDevice(audio, 0);

  SDL_GameController *pad = NULL;
  for (int i = 0; i < SDL_NumJoysticks(); i++) {
    if (SDL_IsGameController(i)) {
      pad = SDL_GameControllerOpen(i);
      if (pad) break;
    }
  }

  int running = 1;
  int paused = 0;
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

    if (!paused) {
      uint32_t input =
          keyboard_input() | controller_input(pad) | (1u << 30);
      (void)RtlRunFrame(input);
      if (g_fail || !SmrpgLastLleResult())
        Die("Super Mario RPG runtime execution failed");
      SmrpgDrawPpuFrame();
      SDL_UpdateTexture(texture, NULL, pixels,
                        kFrameWidth * kBytesPerPixel);
    }

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
    pace_frame(&next_frame_counter, frame_counters);
  }

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
