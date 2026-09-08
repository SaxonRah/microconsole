#ifndef MC_EX_GFX_H
#define MC_EX_GFX_H

#include "gfx.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Helpers shared by the era examples.
 *
 * Nothing here belongs in MicroRender. Colour math, scanline addressing and a
 * sine table are decisions about how *this* set of programs wants to draw;
 * pushing them upstream would give the renderer opinions it has spent its
 * whole design avoiding. They live next to the examples that use them.
 */

/* ---------------------------------------------------------------- */
/* scanline access                                                   */
/* ---------------------------------------------------------------- */

/* Rows of the current tile, in screen space. Every per-scanline effect walks
   this range instead of 0..height, which is what makes one render() correct
   under both a full-height tile and DOS's 16-row bands. */
void mc_rows(const gfx_renderer_t *r, int *out_y0, int *out_y1);

/* Direct write access to one screen row inside the current tile.
   Returns NULL when the row belongs to a different tile. On success *out_x0
   is the first screen x the returned pointer covers and *out_w the count, so
   the caller indexes row[x - *out_x0].

   A raster effect that recomputes something per scanline and then writes a
   whole line of it wants a row pointer, not 320 clipped gfx_draw_pixel()
   calls. Everything else in these examples goes through the normal gfx API. */
gfx_color_t *mc_row(gfx_renderer_t *r, int y, int *out_x0, int *out_w);

/* ---------------------------------------------------------------- */
/* colour                                                            */
/* ---------------------------------------------------------------- */

gfx_color_t mc_rgb_lerp(gfx_color_t a, gfx_color_t b, int t256);
gfx_color_t mc_rgb_scale(gfx_color_t c, int num, int den);
/* Additive and subtractive colour math, saturating per channel. The SNES did
   this in the PPU's colour-math unit; here it is two shifts and a clamp. */
gfx_color_t mc_rgb_add(gfx_color_t a, gfx_color_t b);
gfx_color_t mc_rgb_sub(gfx_color_t a, gfx_color_t b);
/* Average of two RGB565 values without unpacking, the trick every 16-bit
   blitter used: mask the low bit of each field, average the rest. */
gfx_color_t mc_rgb_half(gfx_color_t a, gfx_color_t b);

/* Quantize to a narrower per-channel depth and expand back to RGB565, which is
   how these examples reproduce a period palette on a 16-bit framebuffer.
   bits is 1..5 per channel: RGB222 is the Master System, RGB333 the PC Engine,
   RGB555 the SNES and the Mega Drive's RGB333 sits between them. */
gfx_color_t mc_rgb_quant(gfx_color_t c, int rbits, int gbits, int bbits);

/* ---------------------------------------------------------------- */
/* integer trig and noise                                            */
/* ---------------------------------------------------------------- */

/* 1024 units to the circle, result in Q15 (-32767..32767). Table-driven for
   the same reason snd_note_hz() is: a 386 pays a table read, not a pow().

   int32_t, not int, and that is not decoration. Open Watcom's DOS targets use
   a 16-bit int even in large model, so a Q15 result is right at the edge of
   the type and the very next thing every caller does -- (mc_sin(a) * k) >> 15
   -- overflows it silently. Returning int32_t makes the multiply promote and
   costs 32-bit targets nothing. */
int32_t mc_sin(int angle1024);
int32_t mc_cos(int angle1024);

/* xorshift32. Seeded and deterministic, never rand(), so two targets running
   the same example produce the same picture. */
uint32_t mc_rand(uint32_t *state);

/* ---------------------------------------------------------------- */
/* small drawing conveniences                                        */
/* ---------------------------------------------------------------- */

/* Fill a screen-space rect with a 2x2 ordered dither between two colours.
   Period hardware had no alpha; a checkerboard of two solid colours read as a
   third one through a composite encoder, and every 16-bit console leaned on
   it. mc_rgb_half() is what that looked like after the TV got hold of it. */
void mc_dither_rect(gfx_renderer_t *r, int x, int y, int w, int h,
                    gfx_color_t a, gfx_color_t b, int phase);

/* Text with a one-pixel drop shadow, so a HUD stays legible over a busy
   playfield without needing a panel behind it. */
void mc_text_shadow(gfx_renderer_t *r, int x, int y, const char *text,
                    gfx_color_t color, int scale);

/* Build a width*height sprite from a callback, and fill in the descriptor.
   Every example paints its own art this way: no BMPs, no pack file, nothing to
   check out, which is the same argument mr_game_demo.c and mw_music_demo.c
   make for being generated rather than loaded. */
void mc_make_sprite(gfx_sprite_t *s, gfx_color_t *pixels, int w, int h,
                    gfx_color_t (*paint)(int x, int y, int arg), int arg,
                    gfx_color_t key, int colorkey);

#ifdef __cplusplus
}
#endif

#endif /* MC_EX_GFX_H */
