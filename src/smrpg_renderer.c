#include "smrpg_renderer.h"

#include <limits.h>
#include <stddef.h>

/* The guest always draws a native raster. Only immutable copies are used by
 * this compositor; changing the window size cannot change guest execution,
 * HDMA, OAM capacity, or the contents of the game's circular tilemaps. */
typedef struct RasterLine {
  uint8_t registers[PPU_SAVESTATE_REGS_SIZE];
  uint16_t vram[0x8000], palette[256], oam[256];
  uint8_t high_oam[32];
} RasterLine;
typedef struct SourceFrame {
  uint8_t ram[0x20000], bwram[0x20000], iram[0x800];
  RasterLine lines[224];
  uint32_t stock[256 * 224];
  unsigned captured;
  uint8_t actor_bwram[0x8000], actor_iram[0x800];
} SourceFrame;
static SourceFrame frame;
static SmrpgRendererStats stats;
static Ppu raster;
enum { kMapStride = 256, kMapCells = 256 * 256 };
static uint8_t connected[kMapCells];
static uint16_t tiles[3][kMapCells];
static int map_width, map_height;
static int last_x = -1, last_y = -1, last_area = -1;
typedef struct MapView { int x, y, area; unsigned scroll_x, scroll_y; bool valid; } MapView;
static MapView views[3];
static const uint8_t *rom_data;
static size_t rom_size;
static uint8_t actor_bwram[0x8000], actor_iram[0x800];
static uint16_t actor_pixels[kSmrpgRenderWidth * 224];
typedef struct ActorSubmission {
  uint8_t bwram[0x8000], iram[0x800], oam[544];
  bool valid;
} ActorSubmission;
static ActorSubmission submissions[4];
static unsigned submission_index;
static bool pending_actors;

void SmrpgRendererSetRom(const uint8_t *rom, size_t size) { rom_data = rom; rom_size = size; }
void SmrpgRendererLatchActors(const uint8_t *bwram, size_t size, const uint8_t *iram) {
  memset(actor_bwram, 0, sizeof(actor_bwram));
  if (size > sizeof(actor_bwram)) size = sizeof(actor_bwram);
  if (bwram) memcpy(actor_bwram, bwram, size);
  if (iram) memcpy(actor_iram, iram, sizeof(actor_iram));
  else memset(actor_iram, 0, sizeof(actor_iram));
  pending_actors = true;
}
void SmrpgRendererSubmitActors(const uint8_t *bwram, size_t size) {
  if (!pending_actors || !bwram || size < 0x3660) return;
  pending_actors = false;
  ActorSubmission *s = &submissions[submission_index++ & 3];
  memcpy(s->bwram, actor_bwram, sizeof(s->bwram));
  memcpy(s->iram, actor_iram, sizeof(s->iram));
  memcpy(s->oam, bwram + 0x3440, sizeof(s->oam));
  s->valid = true;
}
void SmrpgRendererSelectActors(const Ppu *ppu) {
  memset(frame.actor_bwram, 0, sizeof(frame.actor_bwram));
  memset(frame.actor_iram, 0, sizeof(frame.actor_iram));
  for (unsigned i = 0; i < 4; ++i) {
    const ActorSubmission *s = &submissions[(submission_index - 1 - i) & 3];
    if (!s->valid || memcmp(ppu->oam, s->oam, 512) ||
        memcmp(ppu->highOam, s->oam + 512, 32)) continue;
    memcpy(frame.actor_bwram, s->bwram, sizeof(s->bwram));
    memcpy(frame.actor_iram, s->iram, sizeof(s->iram));
    return;
  }
}

static unsigned read16(const uint8_t *p, unsigned a) {
  return p[a] | ((unsigned)p[a + 1] << 8);
}
static bool field_line(const Ppu *p) {
  return (p->bgmode & 7) == 1 && (p->screenEnabled[0] & 0x13) == 0x13 &&
         (p->screenWindowed[0] & 0x13) == 0x13 &&
         (p->windowsel & 0xffffff) == 0x330333 &&
         !PPU_bigTiles(p, 0) && !PPU_bigTiles(p, 1);
}
int SmrpgRendererFitWidth(int width, int height) {
  if (width <= 0 || height <= 0) return 256;
  /* 256x224 uses the conventional 4:3 presentation, as the sibling custom
   * renderers do. Keep that pixel aspect while revealing more world. */
  int64_t logical = ((int64_t)width * 192 + height / 2) / height;
  if (logical < 256) logical = 256;
  if (logical > kSmrpgRenderWidth) logical = kSmrpgRenderWidth;
  return ((int)logical + 1) & ~1;
}
void SmrpgRendererReset(void) {
  frame.captured = 0;
  memset(actor_bwram, 0, sizeof(actor_bwram));
  memset(actor_iram, 0, sizeof(actor_iram));
  memset(submissions, 0, sizeof(submissions));
  submission_index = 0;
  pending_actors = false;
  last_x = last_y = last_area = -1;
  memset(views, 0, sizeof(views));
  memset(&stats, 0, sizeof(stats));
}
void SmrpgRendererBeginFrame(const uint8_t *ram, const uint8_t *bwram,
                             size_t bwram_size, const uint8_t *iram) {
  memcpy(frame.ram, ram, sizeof(frame.ram));
  memset(frame.bwram, 0, sizeof(frame.bwram));
  if (bwram_size > sizeof(frame.bwram)) bwram_size = sizeof(frame.bwram);
  if (bwram) memcpy(frame.bwram, bwram, bwram_size);
  if (iram) memcpy(frame.iram, iram, sizeof(frame.iram));
  else memset(frame.iram, 0, sizeof(frame.iram));
  frame.captured = 0;
  memcpy(frame.actor_bwram, actor_bwram, sizeof(actor_bwram));
  memcpy(frame.actor_iram, actor_iram, sizeof(actor_iram));
}
void SmrpgRendererCaptureLine(const Ppu *p, unsigned y) {
  if (y >= 224) return;
  RasterLine *l = &frame.lines[y];
  memcpy(l->registers, p, sizeof(l->registers));
  memcpy(l->vram, p->vram, sizeof(l->vram));
  memcpy(l->palette, p->cgram, sizeof(l->palette));
  memcpy(l->oam, p->oam, sizeof(l->oam));
  memcpy(l->high_oam, p->highOam, sizeof(l->high_oam));
  frame.captured = y + 1;
}
void SmrpgRendererEndFrame(const uint32_t *stock) {
  memcpy(frame.stock, stock, sizeof(frame.stock));
}
bool SmrpgRendererSaveCapture(const char *path) {
  if (!path || frame.captured != 224) return false;
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  const uint32_t header[] = {0x47525053, 2, sizeof(frame)};
  bool ok = fwrite(header, sizeof(header), 1, f) == 1 &&
            fwrite(&frame, sizeof(frame), 1, f) == 1;
  return fclose(f) == 0 && ok;
}
bool SmrpgRendererLoadCapture(const char *path) {
  FILE *f = path ? fopen(path, "rb") : NULL;
  if (!f) return false;
  uint32_t header[3];
  SourceFrame *candidate = malloc(sizeof(*candidate));
  bool ok = candidate && fread(header, sizeof(header), 1, f) == 1 &&
      header[0] == 0x47525053 &&
      ((header[1] == 2 && header[2] == sizeof(frame)) ||
       (header[1] == 1 && header[2] == offsetof(SourceFrame, actor_bwram))) &&
      fread(candidate, header[2], 1, f) == 1 &&
      candidate->captured == 224 && fgetc(f) == EOF;
  fclose(f);
  if (ok) {
    if (header[1] == 1) {
      memcpy(candidate->actor_bwram, candidate->bwram, sizeof(candidate->actor_bwram));
      memcpy(candidate->actor_iram, candidate->iram, sizeof(candidate->actor_iram));
    }
    frame = *candidate; last_area = -1; memset(views, 0, sizeof(views));
  }
  free(candidate);
  return ok;
}
SmrpgRendererStats SmrpgRendererGetStats(void) { return stats; }

static bool signal_tile(unsigned tile) {
  return (tile & 1023) != 0 && (tile & 1023) != 256;
}
static void expand_map(void) {
  unsigned columns = frame.iram[0x148];
  if (columns != 32 && columns != 64 && columns != 128) columns = 64;
  map_width = columns * 2;
  map_height = 8192 / columns;
  memset(tiles, 0, sizeof(tiles));
  for (unsigned l = 0; l < 3; ++l) {
    unsigned map = 0x10000 + l * 0x2000;
    unsigned set = 0x15000 + l * 0x1000;
    for (int y = 0; y < map_height; ++y)
      for (int x = 0; x < map_width; ++x) {
        unsigned cell = (y / 2) * columns + x / 2;
        unsigned block = l == 2 ? frame.ram[map + cell] : read16(frame.ram, map + cell * 2) & 511;
        tiles[l][y * kMapStride + x] = read16(frame.ram,
            set + block * 8 + (y & 1) * 4 + (x & 1) * 2);
      }
  }
}
void SmrpgRendererLatchActor(const uint8_t *bwram, size_t size, const uint8_t *iram, uint16_t address) {
  if (!bwram || address < 0x6000 || address > 0x71e0 || size < 0x3240) return;
  if (!pending_actors) SmrpgRendererLatchActors(bwram, size, iram);
  /* Each actor is drawn before its next simulation update. Preserve that
   * actor's pose at entry, including actors rejected by the native culler. */
  memcpy(actor_bwram + address - 0x4000, bwram + address - 0x4000, 96);
}
static int wrap_coordinate(int value, int start, int end) {
  int size = end - start;
  if (size <= 0) return value;
  int relative = (value - start) % size;
  return start + (relative < 0 ? relative + size : relative);
}
static int map_index(unsigned layer, int x, int y) {
  unsigned bounds = 0x120 + layer * 8, flags = frame.iram[0x119 + layer];
  if (flags & 1) x = wrap_coordinate(x, frame.iram[bounds] * 2, frame.iram[bounds + 4] * 2);
  if (flags & 2) y = wrap_coordinate(y, frame.iram[bounds + 2] * 2, frame.iram[bounds + 6] * 2);
  return x < 0 || y < 0 || x >= map_width || y >= map_height ? -1 : y * kMapStride + x;
}
static unsigned map_score(const RasterLine *l, const Ppu *p, int cx, int cy,
                          unsigned *signal, bool sparse, unsigned layer) {
  *signal = 0;
  if (cx < 0 || cy < 0 || cx >= map_width || cy >= map_height) return 0;
  unsigned score = 0;
  {
    unsigned base = PPU_bgTilemapAdr(p, layer);
    unsigned sx = ((unsigned)p->hScroll[layer] >> 3) & 31;
    unsigned sy = ((unsigned)p->vScroll[layer] >> 3) & 31;
    for (unsigned y = 0; y < 28; y += sparse ? 5 : 1)
      for (unsigned x = 0; x < 32; x += sparse ? 5 : 1) {
        unsigned actual = l->vram[(base + ((sy + y) & 31) * 32 + ((sx + x) & 31)) & 32767];
        int index = map_index(layer, cx + x, cy + y);
        if (index >= 0 && actual == tiles[layer][index]) {
          ++score;
          *signal += signal_tile(actual);
        }
      }
  }
  return score;
}
static bool solve_layer(const RasterLine *l, const Ppu *p, unsigned layer) {
  MapView *v = &views[layer];
  int area = read16(frame.iram, 0x30), cx = v->x, cy = v->y;
  unsigned signal = 0, score = 0;
  if (v->valid && v->area == area) {
    int dx = (int)(((unsigned)p->hScroll[layer] - v->scroll_x + 128) & 255) - 128;
    int dy = (int)(((unsigned)p->vScroll[layer] - v->scroll_y + 128) & 255) - 128;
    cx = (cx * 8 + (int)(v->scroll_x & 7) + dx) / 8;
    cy = (cy * 8 + (int)(v->scroll_y & 7) + dy) / 8;
    score = map_score(l, p, cx, cy, &signal, false, layer);
  }
  if (score < 800 || signal < 32) {
    unsigned best = 0;
    int x0 = 0, x1 = map_width - 1;
    for (int y = 0; y < map_height; ++y)
      for (int x = x0; x <= x1; ++x) {
        unsigned sig, sample = map_score(l, p, x, y, &sig, true, layer);
        unsigned rank = sample * 128 + sig;
        if (rank > best) { best = rank; cx = x; cy = y; }
      }
    score = map_score(l, p, cx, cy, &signal, false, layer);
  }
  stats.match += score; stats.signal += signal; stats.area = area;
  v->valid = score >= 800 && signal >= 32;
  if (v->valid) *v = (MapView){cx, cy, area, p->hScroll[layer], p->vScroll[layer], true};
  return v->valid;
}
static bool solve_map(const RasterLine *l, const Ppu *p) {
  bool a = solve_layer(l, p, 0), b = solve_layer(l, p, 1);
  if (!a && !b) return false;
  for (unsigned layer = 0; layer < 2; ++layer) {
    if (views[layer].valid) continue;
    unsigned signal = 0, base = PPU_bgTilemapAdr(p, layer);
    for (unsigned i = 0; i < 1024; ++i) signal += signal_tile(l->vram[(base + i) & 32767]);
    if (signal >= 32) return false;
  }
  const MapView *v = &views[a ? 0 : 1];
  last_x = v->x; last_y = v->y; last_area = v->area;
  stats.camera_x = last_x * 8 + (v->scroll_x & 7);
  stats.camera_y = last_y * 8 + (v->scroll_y & 7);
  if (p->screenEnabled[0] & 4) {
    if (PPU_bigTiles(p, 2) || !solve_layer(l, p, 2)) return false;
  } else views[2].valid = false;
  return true;
}

static void connect_room(void) {
  /* Follow authored scenery in two dimensions, with no ten-tile reach cap.
   * Full-map assignments can contain several disconnected indoor rooms. */
  uint16_t queue[kMapCells];
  unsigned first = 0, count = 0;
  memset(connected, 0, sizeof(connected));
  int x0 = frame.iram[0x120] * 2, x1 = frame.iram[0x124] * 2;
  if (x1 <= x0 || x1 > map_width) { x0 = 0; x1 = map_width; }
  for (int y = last_y; y < last_y + 29 && y < map_height; ++y)
    for (int x = last_x; x < last_x + 33 && x < map_width; ++x) {
      unsigned i = y * kMapStride + x;
      if (signal_tile(tiles[0][i]) || signal_tile(tiles[1][i])) {
        connected[i] = 1; queue[count++] = i;
      }
    }
  while (first < count) {
    unsigned i = queue[first++];
    for (int dy = -1; dy <= 1; ++dy)
      for (int dx = -1; dx <= 1; ++dx) {
        int x = (int)(i % kMapStride) + dx, y = (int)(i / kMapStride) + dy;
        if (x < x0 || x >= x1 || y < 0 || y >= map_height) continue;
        unsigned j = y * kMapStride + x;
        if (!connected[j] && (signal_tile(tiles[0][j]) || signal_tile(tiles[1][j]))) {
          connected[j] = 1; queue[count++] = j;
        }
      }
  }
}
static unsigned tile_pixel(const uint16_t *vram, unsigned address, int x, int y) {
  unsigned a = (address + y) & 32767, shift = 7 - x;
  unsigned bits = vram[a] >> shift;
  unsigned pixel = (bits & 1) | ((bits >> 7) & 2);
  bits = vram[(a + 8) & 32767] >> shift;
  return pixel | ((bits & 1) << 2) | ((bits >> 5) & 8);
}
static unsigned background(const RasterLine *l, const Ppu *p, unsigned layer, int x, int y) {
  const MapView *v = &views[layer];
  if (!v->valid) return 0;
  int dx = (int)(((unsigned)p->hScroll[layer] - v->scroll_x + 128) & 255) - 128;
  int dy = (int)(((unsigned)p->vScroll[layer] - v->scroll_y + 128) & 255) - 128;
  int wx = v->x * 8 + (v->scroll_x & 7) + dx + x;
  int wy = v->y * 8 + (v->scroll_y & 7) + dy + y;
  int index = map_index(layer, wx < 0 ? (wx - 7) / 8 : wx / 8, wy < 0 ? (wy - 7) / 8 : wy / 8);
  if (index < 0 || (!(frame.iram[0x119 + layer] & 3) && !connected[index])) return 0;
  unsigned tile = tiles[layer][index];
  int tx = wx & 7, ty = wy & 7;
  if (tile & 0x4000) tx = 7 - tx;
  if (tile & 0x8000) ty = 7 - ty;
  unsigned base = ((p->bgTileAdr >> (layer * 4)) & 15) * 4096;
  unsigned pixel;
  if (layer == 2) {
    unsigned bits = l->vram[(base + (tile & 1023) * 8 + ty) & 32767] >> (7 - tx);
    pixel = (bits & 1) | ((bits >> 7) & 2);
  } else pixel = tile_pixel(l->vram, base + (tile & 1023) * 16, tx, ty);
  if (!pixel) return 0;
  unsigned priority = layer == 2 ? (tile & 0x2000 ? ((p->bgmode & 8) ? 15 : 3) : 1) :
                      (tile & 0x2000 ? 12 : 8) - layer;
  return (priority << 12) | (layer << 8) | ((tile >> 10) & 7) * (layer == 2 ? 4 : 16) | pixel;
}
static unsigned subscreen_pattern(const RasterLine *l, const Ppu *p, int x, int y) {
  /* Field BG3 on the subscreen is a repeating effect plane (water/snow),
   * not part of the culled world map. Main-screen dialogue stays native. */
  unsigned sx = (x + p->hScroll[2]) & 511, sy = (y + p->vScroll[2]) & 511;
  unsigned address = PPU_bgTilemapAdr(p, 2) + ((sy >> 3) & 31) * 32 + ((sx >> 3) & 31);
  if ((p->bgXsc[2] & 1) && (sx & 256)) address += 1024;
  if ((p->bgXsc[2] & 2) && (sy & 256)) address += p->bgXsc[2] & 1 ? 2048 : 1024;
  unsigned tile = l->vram[address & 32767], tx = sx & 7, ty = sy & 7;
  if (tile & 0x4000) tx = 7 - tx;
  if (tile & 0x8000) ty = 7 - ty;
  unsigned base = ((p->bgTileAdr >> 8) & 15) * 4096;
  unsigned bits = l->vram[(base + (tile & 1023) * 8 + ty) & 32767] >> (7 - tx);
  unsigned pixel = (bits & 1) | ((bits >> 7) & 2);
  return pixel ? (((tile & 0x2000) ? ((p->bgmode & 8) ? 15 : 3) : 1) << 12) |
                 0x200 | ((tile >> 10) & 7) * 4 | pixel : 0;
}
static bool window(const Ppu *p, unsigned layer, int x) {
  unsigned flags = (p->windowsel >> (layer * 4)) & 15;
  /* Only the field's fully open horizontal aperture is extended. Vertical
   * HDMA/dialogue masks and iris transitions retain their original extent. */
  int left = p->window1left, right = p->window1right;
  if (left == 8 && right == 247) { left = -kSmrpgRenderWidth; right = kSmrpgRenderWidth; }
  bool a = (x >= left && x <= right) != ((flags & 1) != 0);
  bool b = (x >= p->window2left && x <= p->window2right) != ((flags & 4) != 0);
  if (!(flags & 2)) return (flags & 8) && b;
  if (!(flags & 8)) return a;
  switch ((p->wbgobjlog >> (layer * 2)) & 3) {
    case 0: return a || b;
    case 1: return a && b;
    case 2: return a != b;
    default: return a == b;
  }
}
static bool condition(unsigned mode, bool inside) {
  return mode == 3 || (mode == 1 && !inside) || (mode == 2 && inside);
}
static uint32_t color(const RasterLine *l, const Ppu *p, unsigned main, unsigned sub, int x) {
  if (p->inidisp & 128) return 0;
  unsigned rgb = l->palette[main & 255], layer = (main >> 8) & 15;
  bool inside = window(p, 5, x), clipped = condition(p->cgwsel >> 6, inside);
  bool math = !condition((p->cgwsel >> 4) & 3, inside) &&
              ((p->cgadsub & 63) & (1u << layer));
  unsigned other = p->fixedColor;
  bool half = math && (p->cgadsub & 64) && !clipped;
  if (math && (p->cgwsel & 2)) {
    if (sub & 255) other = l->palette[sub & 255];
    else half = false;
  }
  uint32_t result = 0;
  for (int component = 0; component < 3; ++component) {
    int c = clipped ? 0 : (rgb >> (component * 5)) & 31;
    if (math) {
      int second = (other >> (component * 5)) & 31;
      c += p->cgadsub & 128 ? -second : second;
      if (c < 0) c = 0;
      if (half) c /= 2;
      if (c > 31) c = 31;
    }
    c = ((c << 3) | (c >> 2)) * (p->inidisp & 15) / 15;
    result |= (uint32_t)c << (16 - component * 8);
  }
  return result;
}

static bool battle_line(const Ppu *p) {
  /* Retail battle BG1 is a finite 64x32-tile arena uploaded from $7E7000.
   * BG2/BG3 and OBJ contain menus, portraits and combatants; keep those
   * native. Do not mistake the 32x64 title artwork for a wider arena. */
  return (p->bgmode & 7) == 1 && !(p->inidisp & 128) &&
         !PPU_bigTiles(p, 0) && p->bgXsc[0] == 0x41 &&
         p->bgXsc[1] == 0x48 && p->bgXsc[2] == 0x58 &&
         p->bgTileAdr == 0x530 && (p->screenEnabled[0] & 0x11) == 0x11;
}

static unsigned battle_background(const RasterLine *l, const Ppu *p, int x, int y) {
  int camera_x = p->hScroll[0] & 511;
  if (camera_x > 256) camera_x -= 512; /* small negative camera shake */
  int wx = camera_x + x, wy = (p->vScroll[0] + y) & 255;
  if (wx < 0 || wx >= 512) return 0;
  unsigned address = 0x4000 + (wy / 8) * 32 + (wx / 8 & 31) + (wx >= 256 ? 1024 : 0);
  unsigned tile = l->vram[address], tx = wx & 7, ty = wy & 7;
  /* The decompressed arena pads unassigned cells with zero. CHR tile 0
   * can still contain graphics (e.g. the mountain and lava arenas). */
  if (!tile) return 0;
  if (tile & 0x4000) tx = 7 - tx;
  if (tile & 0x8000) ty = 7 - ty;
  unsigned pixel = tile_pixel(l->vram, (tile & 1023) * 16, tx, ty);
  /* A zero pixel is unpainted map space, not permission to extend the
   * backdrop color or wrap the arena into another copy of itself. */
  return pixel ? ((tile & 0x2000 ? 12 : 8) << 12) | ((tile >> 10) & 7) * 16 | pixel : 0;
}

static void draw_battle_margins(uint8_t *out, size_t pitch, int width) {
  int extra = (width - 256) / 2;
  for (int y = 0; y < 224; ++y) {
    const RasterLine *l = &frame.lines[y];
    memcpy(&raster, l->registers, PPU_SAVESTATE_REGS_SIZE);
    if (!battle_line(&raster)) continue;
    ++stats.battle_lines;
    uint32_t *row = (uint32_t *)(out + y * pitch);
    for (int sx = 0; sx < width; ++sx) {
      int x = sx - extra;
      if (x >= 0 && x < 256) continue;
      if ((raster.screenWindowed[0] & 1) && window(&raster, 0, x)) continue;
      unsigned bg = battle_background(l, &raster, x, y + 1);
      if (!bg) continue;
      unsigned sub = raster.screenEnabled[1] & 1 ? bg : 0x500;
      if ((raster.screenWindowed[1] & 1) && window(&raster, 0, x)) sub = 0x500;
      row[sx] = color(l, &raster, bg, sub, x);
      if (row[sx]) ++stats.margin_pixels;
    }
  }
}

static const uint8_t *rom_bytes(unsigned address, size_t size) {
  if (address < 0xc00000) return NULL;
  size_t offset = address - 0xc00000;
  if (!rom_data || offset > rom_size || size > rom_size - offset) return NULL;
  return rom_data + offset;
}
typedef struct ActorArt {
  int x, y;
  unsigned graphics, animation, palette, priority;
  bool mirror;
} ActorArt;

static void actor_tile(const ActorArt *a, unsigned number, int x, int y,
                       bool flip_x, bool flip_y, int extra, int width) {
  if (!number || number > 511) return;
  const uint8_t *gfx = rom_bytes(a->graphics + number * 32, 32);
  if (!gfx) return;
  if (a->mirror) { x = -x - 8; flip_x = !flip_x; }
  x += a->x; y += a->y;
  for (int row = 0; row < 8; ++row) {
    int sy = y + row;
    if (sy < 0 || sy >= 224) continue;
    unsigned gy = flip_y ? 7 - row : row;
    for (int col = 0; col < 8; ++col) {
      int sx = x + col + extra;
      if (sx < 0 || sx >= width) continue;
      unsigned shift = flip_x ? col : 7 - col;
      unsigned pixel = ((gfx[gy * 2] >> shift) & 1) |
                       (((gfx[gy * 2 + 1] >> shift) & 1) << 1) |
                       (((gfx[gy * 2 + 16] >> shift) & 1) << 2) |
                       (((gfx[gy * 2 + 17] >> shift) & 1) << 3);
      if (pixel) {
        unsigned layer = a->palette >= 4 ? 4 : 6;
        actor_pixels[sy * width + sx] = (uint16_t)((a->priority << 12) |
            (layer << 8) | 128 | a->palette * 16 | pixel);
        if (x + col < 8 || x + col >= 248) ++stats.actor_pixels;
      }
    }
  }
}

/* Retail $C0:AC96/AD11: sparse 16x16 molds and bounded copy packets. Graphics
 * are read from the actor's current ROM binding, so offscreen CHR need not
 * have been uploaded into the SNES's sprite tile slots. */
static unsigned actor_packet(const ActorArt *a, unsigned address, int ox, int oy,
                             int extra, int width) {
  const uint8_t *p = rom_bytes(address, 3);
  if (!p || !p[0] || (p[0] & 3) == 2) return 0;
  unsigned bytes = (p[0] & 3) == 1 ? 2 : 1, offset = 3;
  bool fx = (p[0] & 4) != 0, fy = (p[0] & 8) != 0;
  int x = (int8_t)(p[2] + ox), y = (int8_t)(p[1] + oy);
  for (unsigned q = 0; q < 4; ++q) {
    if (!(p[0] & (128 >> q))) continue;
    const uint8_t *id = rom_bytes(address + offset, bytes);
    if (!id) return 0;
    unsigned number = bytes == 2 ? read16(id, 0) & 511 : id[0];
    actor_tile(a, number, x + ((q & 1) ^ fx) * 8,
               y + ((q >> 1) ^ fy) * 8, fx, fy, extra, width);
    offset += bytes;
  }
  return offset;
}
static void actor_mold(const ActorArt *a, unsigned mold, int extra, int width) {
  unsigned address = a->animation + (mold & 32767);
  const uint8_t *p = rom_bytes(address, 1);
  if (!p) return;
  if (mold & 0x8000) {
    /* Retail $C0:AE22: 24/32 pixel grid, with a one-pixel baseline offset. */
    unsigned flags = p[0], columns = flags & 2 ? 4 : 3;
    unsigned rows = flags & 1 ? 4 : 3, offset = flags & 8 ? 3 : 1;
    p = rom_bytes(address, offset + columns * rows);
    if (!p) return;
    unsigned high = flags & 8 ? read16(p, 1) : 0;
    bool fx = (flags & 64) != 0, fy = (flags & 128) != 0;
    int top = flags & 16 ? -27 : flags & 32 ? -29 : -28;
    for (unsigned q = 0; q < columns * rows; ++q) {
      unsigned number = p[offset + q] | (((high >> q) & 1) << 8);
      unsigned x = q % columns, y = q / columns;
      if (fx) x = columns - 1 - x;
      if (fy) y = rows - 1 - y;
      actor_tile(a, number, -(int)columns * 4 + (int)x * 8, top + (int)y * 8,
                 fx, fy, extra, width);
    }
    return;
  }
  for (unsigned piece = 0; piece < 128; ++piece) {
    p = rom_bytes(address, 5);
    if (!p || !p[0]) break;
    if ((p[0] & 3) == 2) {
      unsigned copy = a->animation + read16(p, 3);
      for (unsigned i = 0; i < p[0] >> 4; ++i) {
        unsigned length = actor_packet(a, copy, p[2], p[1], extra, width);
        if (!length) break;
        copy += length;
      }
      address += 5;
    } else {
      unsigned length = actor_packet(a, address, 0, 0, extra, width);
      if (!length) break;
      address += length;
    }
  }
}
static void actors(int extra, int width) {
  memset(actor_pixels, 0, sizeof(actor_pixels));
  const uint8_t *bw = frame.actor_bwram, *ir = frame.actor_iram;
  if (read16(ir, 0x30) != (unsigned)stats.area) return;
  unsigned count = ir[0x100] + ir[0x101];
  if (count > 40) return;
  unsigned slots[43], n = 0;
  unsigned party = ir[0x3f];
  if (party > 3) party = 3;
  for (unsigned i = 0; i < party; ++i) slots[n++] = 0x2000 + i * 96;
  for (unsigned i = 0; i < count; ++i) {
    unsigned slot = 0x22a0 + i * 96;
    if (i >= ir[0x100] && !(bw[slot + 0xe] & 128)) continue;
    slots[n++] = slot;
  }
  /* Background priority still belongs to the SNES attributes. Among actors,
   * the projected ground coordinate supplies the field's depth ordering. */
  for (unsigned i = 1; i < n; ++i) {
    unsigned value = slots[i], j = i;
    while (j && read16(bw, slots[j - 1] + 2) > read16(bw, value + 2)) {
      slots[j] = slots[j - 1]; --j;
    }
    slots[j] = value;
  }
  for (unsigned i = 0; i < n; ++i) {
    const uint8_t *r = bw + slots[i];
    if (!(r[7] & 128) || (r[7] & 8) || (r[0x14] & 64)) continue;
    unsigned binding = (r[0x14] & 7) + r[0x20];
    if (binding >= ir[0x102] || 0x49c + binding * 12 > 0x800) continue;
    const uint8_t *h = ir + 0x490 + binding * 12;
    unsigned table = (h[10] << 16) + read16(h, 6) + (r[0x15] & 63) * 2;
    const uint8_t *entry = rom_bytes(table, 2);
    if (!entry || read16(entry, 0) == 65535) continue;
    int x = (int16_t)((uint16_t)(read16(r, 0) + read16(r, 0x5c)) & 0xfff0) -
            (int16_t)read16(ir, 0x6a);
    int y = (int16_t)(read16(r, 2) & 0xfff0) - (int16_t)read16(r, 0x5a) +
            (int16_t)read16(r, 0x5e) - (int16_t)(read16(r, 4) & 0xfff0) -
            (int16_t)read16(ir, 0x6c);
    ActorArt art = {.x = (x < 0 ? (x - 15) / 16 : x / 16) + 8, .y = (y < 0 ? (y - 15) / 16 : y / 16) + 8,
        .graphics = (h[11] << 16) + read16(h, 8),
        .animation = (h[10] << 16) + read16(h, 2),
        .palette = r[0x1b] & 7,
        .priority = (((r[9] & 64 ? r[0x45] : r[9]) >> 4) & 3) * 4 + 2,
        .mirror = (r[0x15] & 128) != 0};
    actor_mold(&art, read16(entry, 0), extra, width);
    ++stats.actors;
  }
}

void SmrpgRendererDraw(uint8_t *out, size_t pitch, int width) {
  memset(&stats, 0, sizeof(stats));
  if (!out || width < 256 || width > kSmrpgRenderWidth || pitch < (size_t)width * 4 || frame.captured != 224) return;
  int extra = (width - 256) / 2, reference = -1;
  for (int y = 0; y < 224; ++y) {
    uint32_t *row = (uint32_t *)(out + y * pitch);
    memset(row, 0, width * 4);
    memcpy(row + extra, frame.stock + y * 256, 256 * 4);
    memcpy(&raster, frame.lines[y].registers, PPU_SAVESTATE_REGS_SIZE);
    if (reference < 0 && field_line(&raster) && !(raster.inidisp & 128)) reference = y;
  }
  if (!extra) return;
  if (reference < 0) {
    draw_battle_margins(out, pitch, width);
    return;
  }
  const RasterLine *ref = &frame.lines[reference];
  memcpy(&raster, ref->registers, PPU_SAVESTATE_REGS_SIZE);
  expand_map();
  if (!solve_map(ref, &raster)) return;
  connect_room();
  actors(extra, width);
  bool diagnostic_full = getenv("SMRPG_RENDER_DIAGNOSTIC_FULL") != NULL;
  for (int y = 0; y < 224; ++y) {
    const RasterLine *l = &frame.lines[y];
    memcpy(&raster, l->registers, PPU_SAVESTATE_REGS_SIZE);
    if (!field_line(&raster)) continue;
    ++stats.field_lines;
    uint32_t *row = (uint32_t *)(out + y * pitch);
    for (int sx = 0; sx < width; ++sx) {
      int x = sx - extra;
      bool aperture = raster.window1left == 8 && raster.window1right == 247;
      if (x >= (aperture ? 8 : 0) && x < (aperture ? 248 : 256) && !diagnostic_full) continue;
      unsigned bg[3] = {background(l, &raster, 0, x, y + 1), background(l, &raster, 1, x, y + 1),
                        background(l, &raster, 2, x, y + 1)};
      unsigned screens[2] = {0x500, 0x500};
      for (unsigned sub = 0; sub < 2; ++sub)
      {
        for (unsigned layer = 0; layer < 3; ++layer) {
          unsigned bit = 1u << layer;
          if (!(raster.screenEnabled[sub] & bit) ||
              ((raster.screenWindowed[sub] & bit) && window(&raster, layer, x))) continue;
          if (bg[layer] > screens[sub]) screens[sub] = bg[layer];
        }
        if (sub && (raster.screenEnabled[1] & 4) &&
            (!(raster.screenWindowed[1] & 4) || !window(&raster, 2, x))) {
          unsigned pattern = subscreen_pattern(l, &raster, x, y + 1);
          if (pattern > screens[1]) screens[1] = pattern;
        }
        unsigned obj = actor_pixels[y * width + sx];
        if ((raster.screenEnabled[sub] & 16) &&
            (!(raster.screenWindowed[sub] & 16) || !window(&raster, 4, x)) &&
            obj > screens[sub]) screens[sub] = obj;
      }
      row[sx] = color(l, &raster, screens[0], screens[1], x);
      if (row[sx]) ++stats.margin_pixels;
    }
  }
}
