/*
 * MicroConsole DOS frontend for the console-era examples.
 *
 * Structurally the same program as src/dos/main.c: MicroRender renders 16-row
 * RGB565 tiles through gfx_render_tiled_no_clear(), dos_vga_flush_tile()
 * quantizes each tile to the fixed RGB332 palette on its way into the four
 * unchained VGA banks, and mc_sb_service() refills the Sound Blaster's DMA
 * half that is no longer playing, once per frame.
 *
 * The interesting thing about this target is that it is the one that proves
 * the examples are actually portable. Everything else they run on has a 32-bit
 * int; this has a 16-bit one, and an expression that overflows it does not
 * warn, it quietly computes something else. tools/mc_example_capture_dos.c is
 * the answer to that -- it renders the same deterministic frames here and lets
 * the host diff them pixel for pixel and sample for sample.
 *
 * Three differences from the stress frontend, all forced by what these are:
 *
 * The simulation runs on a fixed 60 Hz accumulator rather than one tick per
 * rendered frame. The stress test couples them deliberately, because it is a
 * benchmark and wants the workload to scale with the frame rate. These are
 * animations with an intended speed, and a fast machine should not play them
 * fast.
 *
 * Audio is serviced before and after the render, not just before. A frame here
 * can take considerably longer than the stress scene's, and the Sound Blaster
 * DMA does not wait.
 *
 * Input comes from the INT 9 handler rather than kbhit(), because several
 * examples take held direction keys and the BIOS queue reports typematic
 * repeat rather than key state.
 *
 *   MCEXDEMO [/example ID] [/frames N] [/volume N] [/noaudio] [/list]
 */

#include <conio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dos_keyboard.h"
#include "dos_vga.h"
#include "gfx.h"
#include "mc_example.h"
#include "mr_demo_input.h"
#include "snd.h"

#ifndef MC_DOS_AUDIO
#define MC_DOS_AUDIO 1
#endif

#if MC_DOS_AUDIO
#include "mc_sb.h"
#endif

#define MC_W 320
#define MC_H 240
#define MC_TILE_H 16
#define MC_DEFAULT_FRAMES 0ul

/* Scan codes the stock DOS frontend does not already name. */
#define MC_SC_LBRACKET 0x1A
#define MC_SC_RBRACKET 0x1B
#define MC_SC_MINUS 0x0C
#define MC_SC_EQUALS 0x0D
#define MC_SC_TAB 0x0F
#define MC_SC_Z 0x2C
#define MC_SC_M 0x32

static gfx_renderer_t g_renderer;
static gfx_color_t g_tile[MC_W * MC_TILE_H];
static const mc_example_t GFX_PTR *g_example;

static void draw_example(gfx_renderer_t GFX_PTR *r, void GFX_PTR *user) {
  (void)user;
  if (g_example && g_example->render)
    g_example->render(r);
}

/* The audio clock is never rewound across a switch. A mixer frame is an
   absolute position and the DMA buffer being consumed when the key is pressed
   is already in flight; restarting the count would put a discontinuity in the
   middle of it. mc_sb.c owns the frame counter, so ask it. */
static long mc_audio_frame(void) {
#if MC_DOS_AUDIO
  return (long)mc_sb_frames();
#else
  return 0L;
#endif
}

static void select_example(int index, const snd_mixer_t GFX_PTR *mixer) {
  int count = mc_example_count();
  if (count <= 0)
    return;
  while (index < 0)
    index += count;
  index %= count;
  g_example = mc_example_at(index);
  if (g_example && g_example->init)
    g_example->init(MC_W, MC_H, mixer, mc_audio_frame());
}

static unsigned long parse_ulong_arg(int argc, char **argv, int *i,
                                     unsigned long fallback) {
  char *endp;
  unsigned long value;
  if (*i + 1 >= argc)
    return fallback;
  ++(*i);
  endp = NULL;
  value = strtoul(argv[*i], &endp, 10);
  if (!endp || endp == argv[*i] || *endp != '\0')
    return fallback;
  return value;
}

static void print_usage(void) {
  int i;
  printf("MicroConsole DOS: console-era examples\n");
  printf("Usage: MCEXDEMO [/example ID] [/frames N] [/volume N] [/noaudio]\n");
  printf("\n");
  printf("Controls: [ ] switch example, TAB debug overlay,\n");
  printf("          arrows and Z interact, SPACE sfx, -/+ volume, ESC exit\n");
  printf("\n");
  for (i = 0; i < mc_example_count(); ++i) {
    const mc_example_t *e = mc_example_at(i);
    printf("  %-16s %s\n", e->id, e->system);
  }
}

int main(int argc, char **argv) {
  mr_demo_input_t input;
  unsigned long frames;
  unsigned long frame_limit;
  unsigned long start_tick;
  unsigned long last_tick;
  unsigned long tick_accum;
  unsigned long start_us;
  unsigned long elapsed_us;
  int audio_enabled;
  int audio_ok;
  int volume;
  int index;
  int muted;
  int prev_lb, prev_rb, prev_tab, prev_z, prev_space, prev_m;
  int i;

  frame_limit = MC_DEFAULT_FRAMES;
  audio_enabled = 1;
  audio_ok = 0;
  volume = 100;
  index = 0;
  muted = 0;

  for (i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "/frames") == 0) {
      frame_limit = parse_ulong_arg(argc, argv, &i, frame_limit);
    } else if (strcmp(argv[i], "/volume") == 0) {
      volume = (int)parse_ulong_arg(argc, argv, &i, (unsigned long)volume);
      if (volume < 0)
        volume = 0;
      if (volume > 100)
        volume = 100;
    } else if (strcmp(argv[i], "/noaudio") == 0) {
      audio_enabled = 0;
    } else if (strcmp(argv[i], "/example") == 0 && i + 1 < argc) {
      int found = mc_example_find(argv[++i]);
      if (found < 0) {
        printf("unknown example: %s\n", argv[i]);
        print_usage();
        return 1;
      }
      index = found;
    } else if (strcmp(argv[i], "/list") == 0 || strcmp(argv[i], "/?") == 0) {
      print_usage();
      return 0;
    } else {
      printf("unknown argument: %s\n", argv[i]);
      print_usage();
      return 1;
    }
  }

  gfx_init(&g_renderer, MC_W, MC_H, g_tile, MC_TILE_H, dos_vga_flush_tile, 0);

#if MC_DOS_AUDIO
  if (audio_enabled) {
    audio_ok = mc_sb_init(volume);
    if (!audio_ok)
      printf("WARNING: Sound Blaster init failed; graphics will still run.\n");
  }
#else
  (void)audio_enabled;
#endif

  /* mc_sb owns the mixer on this target, so ask it for one and hand that to
     the example. If the Sound Blaster did not come up there is no mixer at
     all, and every example accepts NULL and falls back to SND_DEFAULT_RATE --
     which is what keeps the graphics running on a machine with no sound
     card. */
#if MC_DOS_AUDIO
  select_example(index, audio_ok ? mc_sb_mixer() : 0);
  if (audio_ok && g_example)
    mc_sb_set_scene(g_example->mix, 0);
#else
  select_example(index, 0);
#endif

  dos_keyboard_install();
  dos_vga_enter();

  start_us = dos_vga_micros();
  start_tick = dos_vga_ticks();
  last_tick = start_tick;
  tick_accum = 0ul;
  frames = 0ul;
  prev_lb = prev_rb = prev_tab = prev_z = prev_space = prev_m = 0;
  memset(&input, 0, sizeof(input));

  while (frame_limit == 0ul || frames < frame_limit) {
    unsigned long now_tick;
    unsigned long dt;
    int lb, rb, tab, z, space, m;
    int steps;

    if (dos_key_down(DOS_SC_ESC))
      break;

#if MC_DOS_AUDIO
    if (audio_ok)
      mc_sb_service();
#endif

    /* Held state for the directions, edges for everything else. */
    input.dx = 0;
    input.dy = 0;
    input.buttons = 0u;
    if (dos_key_down(DOS_SC_LEFT))
      input.dx -= 1;
    if (dos_key_down(DOS_SC_RIGHT))
      input.dx += 1;
    if (dos_key_down(DOS_SC_UP))
      input.dy -= 1;
    if (dos_key_down(DOS_SC_DOWN))
      input.dy += 1;

    z = dos_key_down(MC_SC_Z);
    tab = dos_key_down(MC_SC_TAB);
    space = dos_key_down(DOS_SC_SPACE);
    lb = dos_key_down(MC_SC_LBRACKET);
    rb = dos_key_down(MC_SC_RBRACKET);
    m = dos_key_down(MC_SC_M);

    if (z && !prev_z)
      input.buttons |= MR_DEMO_INPUT_ACTION;
    if (tab && !prev_tab)
      input.buttons |= MR_DEMO_INPUT_DEBUG;

    if ((rb && !prev_rb) || (lb && !prev_lb)) {
      index += (rb && !prev_rb) ? 1 : -1;
#if MC_DOS_AUDIO
      select_example(index, audio_ok ? mc_sb_mixer() : 0);
      if (audio_ok && g_example)
        mc_sb_set_scene(g_example->mix, 0);
#else
      select_example(index, 0);
#endif
    }

#if MC_DOS_AUDIO
    if (audio_ok && space && !prev_space && g_example && g_example->sfx)
      g_example->sfx(mc_sb_mixer(), mc_audio_frame());
    if (audio_ok && m && !prev_m) {
      muted = !muted;
      mc_sb_set_volume(muted ? 0 : volume);
    }
    if (audio_ok && dos_key_down(MC_SC_EQUALS)) {
      volume = (volume + 5 > 100) ? 100 : volume + 5;
      muted = 0;
      mc_sb_set_volume(volume);
    }
    if (audio_ok && dos_key_down(MC_SC_MINUS)) {
      volume = (volume < 5) ? 0 : volume - 5;
      muted = 0;
      mc_sb_set_volume(volume);
    }
#else
    (void)muted;
    (void)volume;
#endif

    prev_lb = lb;
    prev_rb = rb;
    prev_tab = tab;
    prev_z = z;
    prev_space = space;
    prev_m = m;

    /* Fixed 60 Hz simulation off the 18.2 Hz BIOS tick. 60/18.2 is not an
       integer, so the accumulator carries the remainder in 1/1000ths of a
       tick rather than rounding it away every frame -- otherwise the examples
       run about 10% slow and nothing says why. */
    now_tick = dos_vga_ticks();
    dt = now_tick - last_tick;
    last_tick = now_tick;
    tick_accum += dt * 3297ul; /* 60 Hz steps per tick, x1000 */
    steps = 0;
    while (tick_accum >= 1000ul && steps < 8) {
      tick_accum -= 1000ul;
      if (g_example && g_example->tick)
        g_example->tick(&input);
      /* Edge-triggered buttons belong to the first step only, exactly as
         mr_demo_input.h asks. */
      input.buttons &= (uint16_t)~MR_DEMO_INPUT_EDGE_MASK;
      ++steps;
    }
    if (steps == 0 && frames == 0ul) {
      /* Always run at least one step before the first frame, so nothing is
         rendered from uninitialized simulation state. */
      if (g_example && g_example->tick)
        g_example->tick(&input);
    }

    gfx_render_tiled_no_clear(&g_renderer, draw_example, 0);
    ++frames;

#if MC_DOS_AUDIO
    if (audio_ok)
      mc_sb_service();
#endif
  }

  elapsed_us = dos_vga_micros() - start_us;

  dos_vga_leave();
  dos_keyboard_remove();
#if MC_DOS_AUDIO
  if (audio_ok)
    mc_sb_shutdown();
#endif

  printf("done: %s frames=%lu elapsed=%.3f s avg=%.2f fps\n",
         g_example ? g_example->id : "(none)", frames,
         (double)elapsed_us / 1000000.0,
         elapsed_us ? ((double)frames * 1000000.0) / (double)elapsed_us : 0.0);
  (void)start_tick;
  return 0;
}
