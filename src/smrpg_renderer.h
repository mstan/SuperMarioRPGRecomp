#pragma once

#include "snes/ppu.h"

enum { kSmrpgRenderWidth = 1024, kSmrpgRenderHeight = 224 };
typedef struct SmrpgRendererStats {
  unsigned field_lines, margin_pixels, actors, actor_pixels, battle_lines;
  int camera_x, camera_y, area;
  unsigned match, signal;
} SmrpgRendererStats;

void SmrpgRendererReset(void);
void SmrpgRendererSetRom(const uint8_t *rom, size_t size);
void SmrpgRendererLatchActors(const uint8_t *bwram, size_t size, const uint8_t *iram);
void SmrpgRendererLatchActor(const uint8_t *bwram, size_t size, const uint8_t *iram, uint16_t address);
void SmrpgRendererSubmitActors(const uint8_t *bwram, size_t size);
void SmrpgRendererSelectActors(const Ppu *ppu);
void SmrpgRendererBeginFrame(const uint8_t *ram, const uint8_t *bwram,
                             size_t bwram_size, const uint8_t *iram);
void SmrpgRendererCaptureLine(const Ppu *ppu, unsigned y);
void SmrpgRendererEndFrame(const uint32_t *stock);
void SmrpgRendererDraw(uint8_t *out, size_t pitch, int width);
bool SmrpgRendererSaveCapture(const char *path);
bool SmrpgRendererLoadCapture(const char *path);
SmrpgRendererStats SmrpgRendererGetStats(void);
int SmrpgRendererFitWidth(int width, int height);
