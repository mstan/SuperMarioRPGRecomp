#include "smrpg_renderer.h"

int main(int argc, char **argv) {
  if (argc != 5) {
    fprintf(stderr, "usage: smrpg_render_capture ROM capture output.ppm width\n");
    return 2;
  }
  FILE *rom = fopen(argv[1], "rb");
  if (!rom) return 2;
  uint8_t *bytes = malloc(0x400000);
  bool ok = bytes && fread(bytes, 0x400000, 1, rom) == 1 && fgetc(rom) == EOF;
  fclose(rom);
  if (!ok) { free(bytes); return 2; }
  SmrpgRendererSetRom(bytes, 0x400000);
  if (!SmrpgRendererLoadCapture(argv[2])) { free(bytes); return 3; }
  int width = atoi(argv[4]);
  if (width < 256 || width > kSmrpgRenderWidth || (width & 1)) { free(bytes); return 2; }
  uint32_t *pixels = calloc((size_t)width * 224, 4);
  if (!pixels) { free(bytes); return 4; }
  SmrpgRendererDraw((uint8_t *)pixels, width * 4, width);
  FILE *out = fopen(argv[3], "wb");
  if (!out) { free(bytes); free(pixels); return 4; }
  fprintf(out, "P6\n%d 224\n255\n", width);
  for (int i = 0; i < width * 224; ++i) {
    uint8_t rgb[] = {pixels[i] >> 16, pixels[i] >> 8, pixels[i]};
    if (fwrite(rgb, sizeof(rgb), 1, out) != 1) ok = false;
  }
  if (fclose(out)) ok = false;
  SmrpgRendererStats s = SmrpgRendererGetStats();
  printf("{\"width\":%d,\"area\":%d,\"camera\":[%d,%d],\"match\":%u,"
         "\"signal\":%u,\"field_lines\":%u,\"margin_pixels\":%u,"
         "\"actors\":%u,\"actor_pixels\":%u}\n",
         width, s.area, s.camera_x, s.camera_y, s.match, s.signal,
         s.field_lines, s.margin_pixels, s.actors, s.actor_pixels);
  free(bytes); free(pixels);
  return ok ? 0 : 4;
}
