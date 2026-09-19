/* Synthetic fixtures: no copyrighted ROM, generated code or guest execution. */
#include "../src/smrpg_renderer.c"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
static uint8_t ram[0x20000], bw[0x20000], ir[0x800];
static uint32_t native[256 * 224], output[kSmrpgRenderWidth * 224];
static Ppu p;
static void word(uint8_t *memory, unsigned offset, unsigned value) {
  memory[offset] = value; memory[offset + 1] = value >> 8;
}

int main(void) {
  CHECK(SmrpgRendererFitWidth(1920, 1080) == 342);
  CHECK(SmrpgRendererFitWidth(2560, 1080) == 456);
  CHECK(SmrpgRendererFitWidth(3840, 1080) == 684);
  CHECK(SmrpgRendererFitWidth(600, 900) == 256);
  CHECK(SmrpgRendererFitWidth(INT_MAX, 1) == 1024);
  CHECK(SmrpgRendererFitWidth(0, 0) == 256);

  /* The river's 32-column assignment and a 128-column assignment have
   * different row addresses; neither may use the usual 64-column stride. */
  for (unsigned columns = 32; columns <= 128; columns *= 2) {
    memset(ram, 0, sizeof(ram)); memset(ir, 0, sizeof(ir));
    ir[0x148] = columns;
    word(ram, 0x10000 + columns * 2 + 2, 9);
    word(ram, 0x15000 + 9 * 8, 0x1234);
    ram[0x14000 + columns + 1] = 11;
    ram[0x14000 + columns + 2] = 12;
    word(ram, 0x17000 + 11 * 8, 0x4321);
    SmrpgRendererBeginFrame(ram, bw, sizeof(bw), ir);
    expand_map();
    CHECK(tiles[0][2 * kMapStride + 2] == 0x1234);
    CHECK(tiles[2][2 * kMapStride + 2] == 0x4321);
    CHECK(map_width == (int)columns * 2);
  }
  frame.iram[0x119] = 3;
  frame.iram[0x120] = 2; frame.iram[0x124] = 18;
  frame.iram[0x122] = 3; frame.iram[0x126] = 19;
  CHECK(map_index(0, 3, 5) == 37 * kMapStride + 35);
  CHECK(map_index(0, 36, 38) == 6 * kMapStride + 4);
  CHECK(map_index(0, -29, -27) == 37 * kMapStride + 35);

  /* A long room reaches the edge of a 1024-wide viewport; disconnected
   * scenery from a neighboring indoor room must stay hidden. */
  memset(tiles, 0, sizeof(tiles)); memset(frame.iram, 0, sizeof(frame.iram));
  map_width = map_height = 128; last_x = last_y = 0;
  for (int x = 0; x < 120; ++x) tiles[0][x] = 1;
  tiles[0][100 * kMapStride + 100] = 1;
  connect_room();
  CHECK(connected[119]);
  CHECK(!connected[100 * kMapStride + 100]);

  /* BG3 uses byte metatile indices and 2bpp art. Its authored repeating
   * sky must fill field margins while preserving the native interior. */
  SmrpgRendererReset();
  memset(ram, 0, sizeof(ram)); memset(ir, 0, sizeof(ir)); memset(&p, 0, sizeof(p));
  ir[0x148] = 64; ir[0x11b] = 3;
  ir[0x124] = ir[0x12c] = ir[0x134] = 32;
  ir[0x126] = ir[0x12e] = ir[0x136] = 32;
  for (unsigned layer = 0; layer < 3; ++layer)
    for (unsigned q = 0; q < 4; ++q) word(ram, 0x15000 + layer * 0x1000 + q * 2, 1);
  p.inidisp = 15; p.bgmode = 1; p.screenEnabled[0] = p.screenWindowed[0] = 0x17;
  p.windowsel = 0x330333; p.window1left = 8; p.window1right = 247;
  p.bgXsc[0] = 0x10; p.bgXsc[1] = 0; p.bgXsc[2] = 8; p.bgTileAdr = 0x200;
  for (unsigned i = 0; i < 1024; ++i) p.vram[0x1000 + i] = p.vram[i] = p.vram[0x800 + i] = 1;
  /* BG1/BG2 tile 1 shares VRAM with the synthetic map: make it transparent. */
  memset(p.vram + 16, 0, 16 * sizeof(uint16_t));
  for (unsigned y = 0; y < 8; ++y) p.vram[0x2008 + y] = 255;
  p.cgram[1] = 31;
  for (unsigned i = 0; i < 256 * 224; ++i) native[i] = 0x112233;
  SmrpgRendererBeginFrame(ram, bw, sizeof(bw), ir);
  for (unsigned y = 0; y < 224; ++y) SmrpgRendererCaptureLine(&p, y);
  SmrpgRendererEndFrame(native);
  SmrpgRendererDraw((uint8_t *)output, 684 * 4, 684);
  CHECK(stats.field_lines == 224);
  CHECK(output[0] == 0xff0000 && output[683] == 0xff0000);
  CHECK(output[214 + 128] == 0x112233);

  /* A 272-pixel arena contains just eight extra authored pixels per side.
   * Preserve the HUD/actors and leave the unpainted rest of the window black. */
  SmrpgRendererReset(); memset(&p, 0, sizeof(p));
  p.inidisp = 15; p.bgmode = 9; p.screenEnabled[0] = 0x17;
  p.bgXsc[0] = 0x41; p.bgXsc[1] = 0x48; p.bgXsc[2] = 0x58;
  p.bgTileAdr = 0x530; p.hScroll[0] = 520; p.vScroll[0] = 260;
  p.cgram[1] = 31;
  /* Tile 0 deliberately has opaque graphics: zero map entries are padding,
   * not permission to repeat that graphic across the unused half-map. */
  for (unsigned y = 0; y < 8; ++y) p.vram[y] = p.vram[16 + y] = 255;
  for (unsigned y = 0; y < 32; ++y) {
    p.vram[0x4000 + y * 32] = 1;
    p.vram[0x4400 + y * 32 + 1] = 1;
  }
  SmrpgRendererBeginFrame(ram, bw, sizeof(bw), ir);
  for (unsigned y = 0; y < 224; ++y) SmrpgRendererCaptureLine(&p, y);
  SmrpgRendererEndFrame(native);
  SmrpgRendererDraw((uint8_t *)output, 684 * 4, 684);
  CHECK(stats.battle_lines == 224);
  for (unsigned y = 0; y < 224; ++y) {
    CHECK(output[y * 684 + 205] == 0 && output[y * 684 + 478] == 0);
    CHECK(output[y * 684 + 206] == 0xff0000 && output[y * 684 + 477] == 0xff0000);
    for (unsigned x = 0; x < 256; ++x) CHECK(output[y * 684 + 214 + x] == 0x112233);
  }
  /* The title has a taller map, not extra horizontal artwork. */
  p.bgXsc[0] = 0x4a; p.bgTileAdr = 0x4400;
  CHECK(!battle_line(&p));

  /* OAM matching chooses a complete immutable submission. Later guest
   * updates and a different submission cannot change the selected frame. */
  SmrpgRendererReset();
  memset(&p, 0, sizeof(p));
  bw[0x2000] = 17;
  SmrpgRendererLatchActor(bw, sizeof(bw), ir, 0x6000);
  bw[0x2000] = 99;
  SmrpgRendererSubmitActors(bw, sizeof(bw));
  SmrpgRendererSelectActors(&p);
  CHECK(frame.actor_bwram[0x2000] == 17);
  p.oam[0] = 123;
  SmrpgRendererSelectActors(&p);
  CHECK(frame.actor_bwram[0x2000] == 0);

  /* Unsupported scenes retain every native pixel at its exact offset. */
  SmrpgRendererBeginFrame(ram, bw, sizeof(bw), ir);
  for (unsigned y = 0; y < 224; ++y) SmrpgRendererCaptureLine(&p, y);
  for (unsigned i = 0; i < 256 * 224; ++i) native[i] = i;
  SmrpgRendererEndFrame(native);
  memset(native, 0, sizeof(native));
  SmrpgRendererDraw((uint8_t *)output, 684 * 4, 684);
  for (unsigned y = 0; y < 224; ++y) {
    CHECK(output[y * 684 + 213] == 0);
    for (unsigned x = 0; x < 256; ++x) CHECK(output[y * 684 + 214 + x] == y * 256 + x);
    CHECK(output[y * 684 + 470] == 0);
  }
  p.windowsel = 0x330333; p.window1left = 8; p.window1right = 247;
  CHECK(!window(&p, 0, -100));
  p.window1left = 40; p.window1right = 200;
  CHECK(window(&p, 0, -100)); CHECK(!window(&p, 0, 100));

  CHECK(SmrpgRendererSaveCapture("renderer-test.srpg"));
  CHECK(SmrpgRendererLoadCapture("renderer-test.srpg"));
  FILE *bad = fopen("renderer-test.srpg", "wb"); CHECK(bad != NULL); fclose(bad);
  CHECK(!SmrpgRendererLoadCapture("renderer-test.srpg"));
  CHECK(frame.captured == 224);
  remove("renderer-test.srpg");
  SmrpgRendererReset(); CHECK(frame.captured == 0); CHECK(!submissions[0].valid);
  puts("renderer tests: PASS");
  return 0;
}
