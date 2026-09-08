#include "mc_ex_gfx.h"

/* Quarter-wave sine, Q15, 257 entries so that index 256 (a quarter turn) is
   exact rather than interpolated. 1024 units to the circle matches the
   angular resolution these examples actually need: one unit is about a third
   of a degree, which is finer than a 320-pixel-wide screen can show. */
static const int16_t mc_sin_q[257] = {
         0,    201,    402,    603,    804,   1005,   1206,   1407,
      1608,   1809,   2009,   2210,   2410,   2611,   2811,   3012,
      3212,   3412,   3612,   3811,   4011,   4210,   4410,   4609,
      4808,   5007,   5205,   5404,   5602,   5800,   5998,   6195,
      6393,   6590,   6786,   6983,   7179,   7375,   7571,   7767,
      7962,   8157,   8351,   8545,   8739,   8933,   9126,   9319,
      9512,   9704,   9896,  10087,  10278,  10469,  10659,  10849,
     11039,  11228,  11417,  11605,  11793,  11980,  12167,  12353,
     12539,  12725,  12910,  13094,  13279,  13462,  13645,  13828,
     14010,  14191,  14372,  14553,  14732,  14912,  15090,  15269,
     15446,  15623,  15800,  15976,  16151,  16325,  16499,  16673,
     16846,  17018,  17189,  17360,  17530,  17700,  17869,  18037,
     18204,  18371,  18537,  18703,  18868,  19032,  19195,  19357,
     19519,  19680,  19841,  20000,  20159,  20317,  20475,  20631,
     20787,  20942,  21096,  21250,  21403,  21554,  21705,  21856,
     22005,  22154,  22301,  22448,  22594,  22739,  22884,  23027,
     23170,  23311,  23452,  23592,  23731,  23870,  24007,  24143,
     24279,  24413,  24547,  24680,  24811,  24942,  25072,  25201,
     25329,  25456,  25582,  25708,  25832,  25955,  26077,  26198,
     26319,  26438,  26556,  26674,  26790,  26905,  27019,  27133,
     27245,  27356,  27466,  27575,  27683,  27790,  27896,  28001,
     28105,  28208,  28310,  28411,  28510,  28609,  28706,  28803,
     28898,  28992,  29085,  29177,  29268,  29358,  29447,  29534,
     29621,  29706,  29791,  29874,  29956,  30037,  30117,  30195,
     30273,  30349,  30424,  30498,  30571,  30643,  30714,  30783,
     30852,  30919,  30985,  31050,  31113,  31176,  31237,  31297,
     31356,  31414,  31470,  31526,  31580,  31633,  31685,  31736,
     31785,  31833,  31880,  31926,  31971,  32014,  32057,  32098,
     32137,  32176,  32213,  32250,  32285,  32318,  32351,  32382,
     32412,  32441,  32469,  32495,  32521,  32545,  32567,  32589,
     32609,  32628,  32646,  32663,  32678,  32692,  32705,  32717,
     32728,  32737,  32745,  32752,  32757,  32761,  32765,  32766,
     32767
};

void mc_rows(const gfx_renderer_t *r, int *out_y0, int *out_y1) {
  int y0 = 0;
  int y1 = 0;
  if (r) {
    y0 = r->tile_y;
    y1 = r->tile_y + r->tile_h;
    if (y0 < 0)
      y0 = 0;
    if (y1 > r->height)
      y1 = r->height;
    if (y1 < y0)
      y1 = y0;
  }
  if (out_y0)
    *out_y0 = y0;
  if (out_y1)
    *out_y1 = y1;
}

gfx_color_t *mc_row(gfx_renderer_t *r, int y, int *out_x0, int *out_w) {
  if (!r || !r->tile)
    return 0;
  if (y < r->tile_y || y >= r->tile_y + r->tile_h)
    return 0;
  if (r->tile_w <= 0)
    return 0;
  if (out_x0)
    *out_x0 = r->tile_x;
  if (out_w)
    *out_w = r->tile_w;
  return r->tile + (long)(y - r->tile_y) * (long)r->tile_stride;
}

/* RGB565 field extraction. Written out rather than macro'd because the shift
   pairs differ per channel and a wrong one is invisible until something is
   subtly the wrong hue. */
#define MC_R5(c) ((int)(((c) >> 11) & 0x1F))
#define MC_G6(c) ((int)(((c) >> 5) & 0x3F))
#define MC_B5(c) ((int)((c) & 0x1F))
#define MC_PACK(r, g, b)                                                       \
  ((gfx_color_t)((((unsigned)(r) & 0x1Fu) << 11) |                             \
                 (((unsigned)(g) & 0x3Fu) << 5) | ((unsigned)(b) & 0x1Fu)))

gfx_color_t mc_rgb_lerp(gfx_color_t a, gfx_color_t b, int t256) {
  int r, g, bl;
  if (t256 <= 0)
    return a;
  if (t256 >= 256)
    return b;
  r = MC_R5(a) + (((MC_R5(b) - MC_R5(a)) * t256) >> 8);
  g = MC_G6(a) + (((MC_G6(b) - MC_G6(a)) * t256) >> 8);
  bl = MC_B5(a) + (((MC_B5(b) - MC_B5(a)) * t256) >> 8);
  return MC_PACK(r, g, bl);
}

gfx_color_t mc_rgb_scale(gfx_color_t c, int num, int den) {
  int r, g, b;
  if (den <= 0)
    return c;
  if (num < 0)
    num = 0;
  r = MC_R5(c) * num / den;
  g = MC_G6(c) * num / den;
  b = MC_B5(c) * num / den;
  if (r > 31)
    r = 31;
  if (g > 63)
    g = 63;
  if (b > 31)
    b = 31;
  return MC_PACK(r, g, b);
}

gfx_color_t mc_rgb_add(gfx_color_t a, gfx_color_t b) {
  int r = MC_R5(a) + MC_R5(b);
  int g = MC_G6(a) + MC_G6(b);
  int bl = MC_B5(a) + MC_B5(b);
  if (r > 31)
    r = 31;
  if (g > 63)
    g = 63;
  if (bl > 31)
    bl = 31;
  return MC_PACK(r, g, bl);
}

gfx_color_t mc_rgb_sub(gfx_color_t a, gfx_color_t b) {
  int r = MC_R5(a) - MC_R5(b);
  int g = MC_G6(a) - MC_G6(b);
  int bl = MC_B5(a) - MC_B5(b);
  if (r < 0)
    r = 0;
  if (g < 0)
    g = 0;
  if (bl < 0)
    bl = 0;
  return MC_PACK(r, g, bl);
}

gfx_color_t mc_rgb_half(gfx_color_t a, gfx_color_t b) {
  /* (a + b) / 2 per field, computed on the packed value: drop the low bit of
     every field with the mask, add, then halve. One AND, one add, one shift,
     and no unpacking -- which is why every console that could average two
     layers at all could afford to do it per pixel. */
  return (gfx_color_t)(((a & 0xF7DEu) >> 1) + ((b & 0xF7DEu) >> 1));
}

gfx_color_t mc_rgb_quant(gfx_color_t c, int rbits, int gbits, int bbits) {
  int r, g, b;
  if (rbits < 1)
    rbits = 1;
  if (gbits < 1)
    gbits = 1;
  if (bbits < 1)
    bbits = 1;
  if (rbits > 5)
    rbits = 5;
  if (gbits > 6)
    gbits = 6;
  if (bbits > 5)
    bbits = 5;
  /* Truncate to the hardware's depth, then replicate the high bits downward so
     the brightest quantized level still reaches full scale. Shifting up and
     leaving zeros would make white come out grey, which is the usual way a
     palette conversion looks wrong. */
  r = MC_R5(c) >> (5 - rbits);
  g = MC_G6(c) >> (6 - gbits);
  b = MC_B5(c) >> (5 - bbits);
  r = (r * 31) / ((1 << rbits) - 1);
  g = (g * 63) / ((1 << gbits) - 1);
  b = (b * 31) / ((1 << bbits) - 1);
  return MC_PACK(r, g, b);
}

int32_t mc_sin(int angle1024) {
  int a = angle1024 & 1023;
  if (a < 256)
    return mc_sin_q[a];
  if (a < 512)
    return mc_sin_q[512 - a];
  if (a < 768)
    return -mc_sin_q[a - 512];
  return -mc_sin_q[1024 - a];
}

int32_t mc_cos(int angle1024) { return mc_sin(angle1024 + 256); }

uint32_t mc_rand(uint32_t *state) {
  uint32_t x;
  if (!state)
    return 0u;
  x = *state ? *state : 0x2545F491u;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x;
  return x;
}

void mc_dither_rect(gfx_renderer_t *r, int x, int y, int w, int h,
                    gfx_color_t a, gfx_color_t b, int phase) {
  int py, px, y0, y1;
  if (!r || w <= 0 || h <= 0)
    return;
  mc_rows(r, &y0, &y1);
  if (y < y0)
    y = y0;
  if (y + h > y1)
    h = y1 - y;
  for (py = y; py < y + h; ++py) {
    int x0, tw;
    gfx_color_t *row = mc_row(r, py, &x0, &tw);
    int lo = x < x0 ? x0 : x;
    int hi = x + w > x0 + tw ? x0 + tw : x + w;
    if (!row)
      continue;
    for (px = lo; px < hi; ++px)
      row[px - x0] = (((px + py + phase) & 1) != 0) ? a : b;
  }
}

void mc_text_shadow(gfx_renderer_t *r, int x, int y, const char *text,
                    gfx_color_t color, int scale) {
  gfx_draw_text5x7(r, x + 1, y + 1, text, GFX_RGB565_BLACK, scale);
  gfx_draw_text5x7(r, x, y, text, color, scale);
}

void mc_make_sprite(gfx_sprite_t *s, gfx_color_t *pixels, int w, int h,
                    gfx_color_t (*paint)(int x, int y, int arg), int arg,
                    gfx_color_t key, int colorkey) {
  int x, y;
  if (!s || !pixels || !paint || w <= 0 || h <= 0)
    return;
  for (y = 0; y < h; ++y)
    for (x = 0; x < w; ++x)
      pixels[y * w + x] = paint(x, y, arg);
  s->width = w;
  s->height = h;
  s->pixels = pixels;
  s->runs = 0;
  s->run_count = 0;
  s->row_start = 0;
  s->key = key;
  s->flags = colorkey ? (uint8_t)GFX_SPRITE_COLORKEY : (uint8_t)0u;
}
