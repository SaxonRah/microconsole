#ifndef MC_EXAMPLE_H
#define MC_EXAMPLE_H

#include "gfx.h"
#include "mr_demo_input.h"
#include "snd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One console-era technique per example.
 *
 * These are MicroConsole programs, not MicroRender modes. MicroRender still
 * only knows how to rasterize RGB565 into a tile, and MicroWave still only
 * knows how to fill a mix block; nothing here adds a mode register, a
 * scanline callback, or a chip emulation to either engine. Every effect below
 * is assembled out of the primitives the two engines already ship, in the
 * layer that is allowed to have policy -- which is exactly the split
 * docs/ARCHITECTURE.md draws.
 *
 * That constraint is the point of the exercise. A raster split is what you get
 * when the thing driving the renderer is willing to change its mind between
 * scanlines. Mode 7 is what you get when it recomputes an affine matrix per
 * row. Neither needs the renderer to know the word "mode".
 *
 * ---------------------------------------------------------------------------
 * Contract
 *
 *   init(w, h, m, start_frame)
 *       Build every sprite, tile and wavetable the example needs. Called once
 *       when the example is selected, and again if it is selected a second
 *       time. Must not allocate: everything lives in file-scope storage, the
 *       same rule mr_stress_test_t and mw_demo_t follow. `start_frame` is the
 *       absolute mixer frame audio should be scheduled from, because the
 *       frontend's audio clock keeps running across a switch.
 *
 *   tick(input)
 *       One 60 Hz simulation step. Deterministic and integer-only, so a DOS
 *       build and a CI runner walk the same states.
 *
 *   render(r)
 *       Draw the current frame. Two requirements, both of which the capture
 *       tool checks:
 *
 *       It must be safe to call once against a full-height tile (the Raylib
 *       and Pico frontends) and repeatedly against 16-row tiles (the DOS
 *       frontend), because it is used both as a direct call and as a
 *       gfx_render_tiled*() draw_scene callback. Use mc_row() / mc_rows() to
 *       touch only the rows the current tile owns, and keep no state that
 *       outlives a row -- a counter accumulated while drawing reads
 *       differently in band three than it does on a full-height tile.
 *
 *       It must write every pixel of every row it is handed. The DOS frontend
 *       uses gfx_render_tiled_no_clear() over a single reused band buffer, so
 *       a pixel nobody wrote shows whatever the previous band left in it. An
 *       example with a border paints the border.
 *
 *   Two arithmetic rules apply throughout, and they exist because one of the
 *   supported targets has a 16-bit int:
 *
 *       Anything that can hold a full-scale sample or a Q15 trig result is
 *       int32_t, never int. A full-scale sample fills a 16-bit int exactly,
 *       and the gain multiply that always follows it does not fit.
 *
 *       Shift before multiplying, so no intermediate leaves 32 bits. Written
 *       the obvious way, a per-scanline projection term reached 1e11 -- fine
 *       on a host where long is 64-bit, wrong on DOS and wrong on the RP2350.
 *
 *   tools/mc_example_capture_dos.c renders the same frames on DOS so the host
 *   can diff them. Neither rule is enforceable by the compiler; both are
 *   checkable by that diff.
 *
 *   mix(m, user)
 *       Fill the current mix block. Signature matches snd_render_one_block()'s
 *       mix_scene so the frontend can hand it over directly. Anything that
 *       generates samples goes through snd_touch_block() and snd_block_add(),
 *       never m->block, exactly as snd.h asks.
 *
 *   sfx(m, at_frame)
 *       Optional one-shot, wired to the same key the stock MicroConsole demo
 *       uses for its blip. NULL if the example has nothing to fire.
 *
 * State is file-scope inside each example rather than caller-owned. The
 * engines are caller-owned because they are libraries; these are programs, and
 * a registry of nine differently-shaped state structs would need either a
 * union big enough for the worst one or the malloc neither engine permits.
 */

#define MC_EX_W 320
#define MC_EX_H 240

typedef struct mc_example {
  const char *id;        /* command-line name, e.g. "nes-split"        */
  const char *system;    /* hardware and year                          */
  const char *technique; /* what the display is doing                  */
  const char *audio;     /* what the sound chip is doing               */
  const char *const *notes; /* NULL-terminated help lines, may be NULL */

  void (*init)(int screen_w, int screen_h, const snd_mixer_t *m,
               long start_frame);
  void (*tick)(const mr_demo_input_t *input);
  void (*render)(gfx_renderer_t *r);
  void (*mix)(snd_mixer_t *m, void *user);
  void (*sfx)(const snd_mixer_t *m, long at_frame);
} mc_example_t;

/* Registry. Ordered oldest hardware first, so stepping through it walks
   forward through the two generations. */
int mc_example_count(void);
const mc_example_t *mc_example_at(int index);
/* -1 when no example carries that id. */
int mc_example_find(const char *id);

/* Accessors, one per translation unit. Declared here so each example file can
   see a prototype for the symbol it defines, which is what
   -Wmissing-prototypes wants and what the engines build with. */
const mc_example_t *mc_example_a2600_beam(void);
const mc_example_t *mc_example_nes_split(void);
const mc_example_t *mc_example_sms_lock(void);
const mc_example_t *mc_example_gb_window(void);
const mc_example_t *mc_example_pce_wavetable(void);
const mc_example_t *mc_example_md_linescroll(void);
const mc_example_t *mc_example_snes_mode7(void);
const mc_example_t *mc_example_snes_colormath(void);
const mc_example_t *mc_example_neogeo_zoom(void);

#ifdef __cplusplus
}
#endif

#endif /* MC_EXAMPLE_H */
