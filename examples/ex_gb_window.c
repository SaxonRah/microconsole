/* Nintendo Game Boy (DMG), 1989 -- LR35902 PPU + APU
 *
 * Display: the window, the STAT interrupt, and a display that is slow enough
 * to be part of the art.
 *
 *   The window is the DMG's second background. It is not a sprite layer and it
 *   does not scroll: it is a whole second tile map that starts at WX,WY and
 *   covers everything below and right of that point, unconditionally. Move WY
 *   down a line at a time and a dialogue box slides up from the bottom of the
 *   screen for the cost of one register write per frame. Nearly every text box
 *   on the machine is this.
 *
 *   The STAT interrupt fires when the LCD controller reaches a chosen line, and
 *   a handler that rewrites SCX before the line is drawn gets a per-scanline
 *   horizontal offset. Feed it a sine and the background ripples -- the water
 *   and heat-haze effect that shows up everywhere from Link's Awakening
 *   onward. It costs an interrupt per scanline, which on a 4 MHz CPU is most
 *   of the frame, so games used it on a band of the screen rather than all of
 *   it. That is what happens here.
 *
 *   BGP is the third piece. There are four shades, and BGP is four two-bit
 *   fields deciding which shade each of the four tile colours maps to. Nothing
 *   in video RAM has to change to fade the screen to white or to black -- just
 *   rotate the fields. It is the cheapest transition on the system and it is
 *   why DMG games fade rather than cut.
 *
 *   And then the panel itself. DMG pixels take milliseconds to settle, so
 *   anything moving leaves a trail. Developers did not fight this; they used it
 *   as free motion blur, and some effects only read correctly *because* of it.
 *   The LCD response is simulated here as a first-order lag per pixel, and it
 *   is why the ripple looks smooth rather than steppy.
 *
 *   All of that is computed in tick() and merely presented in render(). That
 *   split is not incidental: ghosting needs the previous frame, which is state
 *   that must not live inside a function the DOS frontend calls once per 16-row
 *   band.
 *
 * Audio: four channels, and the one that is not like the others.
 *
 *   Channels 1 and 2 are pulses with four duties and a hardware volume
 *   envelope -- which the Master System's PSG did not have, and the difference
 *   is immediately audible: a DMG note can decay smoothly without the CPU
 *   touching it. Channel 1 also has a hardware frequency sweep.
 *
 *   Channel 3 is the interesting one. It plays 32 four-bit samples out of
 *   wave RAM, and the program can write whatever it likes in there. That is a
 *   wavetable synthesiser on a handheld in 1989. Rewriting wave RAM while the
 *   channel plays morphs the timbre continuously, which is what this example
 *   does -- the table crossfades between a sine, a ramp and a hollow pulse
 *   about once a bar. The catch is the volume control: two bits, giving full,
 *   half, quarter and mute, and nothing in between.
 *
 *   Channel 4's shift register can be switched from fifteen stages to seven.
 *   Seven stages is a 127-step sequence, short enough to hear as a pitch
 *   rather than as noise, and it is where every metallic clank and laser on
 *   the machine came from.
 */

#include "mc_ex_chip.h"
#include "mc_ex_gfx.h"
#include "mc_example.h"
#include "mr_strbuf.h"

#include <string.h>

#define GB_W 160
#define GB_H 144
#define GB_X0 ((MC_EX_W - GB_W) / 2)
#define GB_Y0 40
#define GB_MAP 32     /* the DMG's tile maps are 32x32 */
#define GB_TILES 13
#define GB_RIPPLE_TOP 88 /* the band the STAT handler is armed over */

/* The four shades of the original DMG panel: a green LCD, not a grey one. */
static const gfx_color_t gb_shade[4] = {
    GFX_RGB565(0x9B, 0xBC, 0x0F), GFX_RGB565(0x8B, 0xAC, 0x0F),
    GFX_RGB565(0x30, 0x62, 0x30), GFX_RGB565(0x0F, 0x38, 0x0F)};

static struct {
  unsigned long frame;
  int scx, scy;
  int wx, wy;      /* window origin */
  int window_on;
  int bgp;         /* four 2-bit fields: colour index -> shade */
  int fade_phase;
  int ripple;
  int show_debug;

  uint8_t tiles[GB_TILES][8 * 8]; /* 2 bits per pixel */
  uint8_t bg_map[GB_MAP * GB_MAP];
  uint8_t win_map[GB_MAP * GB_MAP];

  uint8_t shade[GB_H][GB_W];  /* what the PPU produced this frame */
  uint8_t lcd[GB_H][GB_W];    /* what the panel is actually showing, 0..255 */
} gb;

static void gb_build_tiles(void) {
  int t, x, y;
  for (t = 0; t < GB_TILES; ++t) {
    for (y = 0; y < 8; ++y) {
      for (x = 0; x < 8; ++x) {
        int v = 0;
        switch (t) {
        case 0:
          v = 0;
          break;
        case 1: /* sand speckle */
          v = (((x * 7 + y * 3) & 15) < 2) ? 1 : 0;
          break;
        case 2: /* water */
          v = (((x + y) & 3) < 2) ? 1 : 2;
          break;
        case 3: /* deep water */
          v = (((x * 3 + y) & 3) < 2) ? 2 : 3;
          break;
        case 4: /* palm trunk */
          v = (x >= 3 && x <= 4) ? 3 : ((x == 2 || x == 5) ? 2 : 0);
          break;
        case 5: /* foliage */
          v = ((x - 4) * (x - 4) + (y - 4) * (y - 4) < 16) ? 2 : 0;
          break;
        case 6: /* rock */
          v = (y == 0 || x == 0) ? 1 : ((y == 7 || x == 7) ? 3 : 2);
          break;
        case 7: /* window frame: top */
          v = (y == 0 || y == 1) ? 3 : 0;
          break;
        case 8: /* window fill */
          v = 0;
          break;
        case 9: /* window frame: left */
          v = (x == 0 || x == 1) ? 3 : 0;
          break;
        case 10: /* window corner */
          v = (x < 2 || y < 2) ? 3 : 0;
          break;
        case 11: /* a line of "text" for the dialogue box */
          v = (y >= 2 && y <= 5 && ((x * 3 + y * 7) % 5) < 2) ? 3 : 0;
          break;
        default:
          v = ((x ^ y) & 3);
          break;
        }
        gb.tiles[t][y * 8 + x] = (uint8_t)v;
      }
    }
  }
}

static void gb_build_maps(void) {
  int x, y;
  uint32_t rng = 0xDEA17u;
  for (y = 0; y < GB_MAP; ++y) {
    for (x = 0; x < GB_MAP; ++x) {
      int t;
      if (y >= 14)
        t = ((y + x) & 1) ? 3 : 2; /* the sea, which is what will ripple */
      else if (y == 13)
        t = 1;
      else if ((x % 7) == 3 && y >= 9 && y < 13)
        t = 4;
      else if (((x % 7) >= 2 && (x % 7) <= 4) && y == 8)
        t = 5;
      else if ((mc_rand(&rng) & 15u) < 2u && y > 4)
        t = 6;
      else
        t = ((mc_rand(&rng) & 7u) < 2u) ? 1 : 0;
      gb.bg_map[y * GB_MAP + x] = (uint8_t)t;

      /* The window map is a separate tile map covering the whole plane. Only
         the part below and right of WX,WY is ever fetched. */
      /* A framed box with three lines of text in it. The window has no
         transparency: every tile it covers replaces the background outright. */
      if (y == 0)
        gb.win_map[y * GB_MAP + x] = 7;
      else if (x == 0)
        gb.win_map[y * GB_MAP + x] = 9;
      else if ((y == 1 || y == 3 || y == 4) && x >= 1 && x < 18)
        gb.win_map[y * GB_MAP + x] = 11;
      else
        gb.win_map[y * GB_MAP + x] = 8;
    }
  }
}

/* BGP: colour index in, shade out. Two bits per index, four indices, one
   register. */
static int gb_bgp_map(int index) { return (gb.bgp >> ((index & 3) * 2)) & 3; }

/* ---------------------------------------------------------------- */
/* APU                                                               */
/* ---------------------------------------------------------------- */

typedef struct gb_pulse {
  mc_osc_t osc;
  int32_t hz_q8;
  int duty;      /* 0..3 -> 12.5, 25, 50, 75 percent */
  int vol;       /* 0..15 */
  int env_period; /* in 64 Hz frame-sequencer steps; 0 disables */
  int env_acc;
  int env_dir;
  int sweep_period;
  int sweep_shift;
  int sweep_acc;
  int on;
} gb_pulse_t;

static struct {
  int rate;
  long seq_acc;
  long seq_period; /* the APU's own 512 Hz frame sequencer */
  int seq_step;

  long row_acc;
  long row_period;
  int row;

  gb_pulse_t p1, p2;

  /* Channel 3. Thirty-two four-bit samples and a two-bit output level. */
  uint8_t wave[32];
  uint8_t wave_target[32];
  mc_osc_t wave_osc;
  int wave_level; /* 0 mute, 1 full, 2 half, 3 quarter -- the register order */
  int wave_on;
  int wave_shape;

  mc_noise_t noise;
  int noise_vol;
  int noise_env_acc;
  int noise_short;
} apu;

static const uint16_t gb_duty[4] = {8192u, 16384u, 32768u, 49152u};

/* Three wave-RAM shapes the example crossfades between. Writing these while
   the channel is running is the whole point of the channel. */
static void gb_wave_shape(uint8_t *dst, int shape) {
  int i;
  for (i = 0; i < 32; ++i) {
    int32_t v;
    switch (shape) {
    case 0: /* sine, quantized to the four bits wave RAM actually holds */
      v = 8 + ((mc_wave_sine((uint32_t)i << 27) * 7) >> 15);
      break;
    case 1: /* ramp */
      v = i / 2;
      break;
    case 2: /* hollow pulse: a narrow duty with a step in it */
      v = (i < 6) ? 15 : ((i < 12) ? 9 : ((i < 20) ? 2 : 5));
      break;
    default: /* two-octave stack: a fundamental with its own octave folded in */
      v = 8 + ((mc_wave_sine((uint32_t)i << 27) * 4) >> 15) +
          ((mc_wave_sine((uint32_t)i << 28) * 3) >> 15);
      break;
    }
    if (v < 0)
      v = 0;
    if (v > 15)
      v = 15;
    dst[i] = (uint8_t)v;
  }
}

static const uint8_t gb_lead[32] = {72, 0,  74, 0,  76, 0,  0,  79,
                                    0,  0,  76, 0,  74, 0,  0,  0,
                                    71, 0,  74, 0,  76, 0,  0,  72,
                                    0,  0,  69, 0,  0,  0,  0,  0};
static const uint8_t gb_harm[32] = {64, 0, 0, 0, 67, 0, 0, 0, 64, 0, 0,
                                    0,  62, 0, 0, 0, 59, 0, 0, 0, 62, 0,
                                    0,  0,  57, 0, 0, 0, 0, 0, 0, 0};
static const uint8_t gb_wavebass[32] = {48, 0, 0,  0,  48, 0, 55, 0,
                                        53, 0, 0,  0,  53, 0, 60, 0,
                                        47, 0, 0,  0,  47, 0, 54, 0,
                                        52, 0, 0,  0,  52, 0, 0,  0};
/* 1 = short-LFSR metallic tick, 2 = long-LFSR snare */
static const uint8_t gb_drum[32] = {2, 0, 1, 0, 2, 0, 1, 1, 2, 0, 1,
                                    0, 2, 0, 1, 1, 2, 0, 1, 0, 2, 0,
                                    1, 1, 2, 0, 1, 0, 2, 1, 1, 1};

static void gb_apu_init(const snd_mixer_t *m) {
  memset(&apu, 0, sizeof(apu));
  apu.rate = m ? m->rate : SND_DEFAULT_RATE;
  /* 512 Hz, and every timed thing in the chip is a division of it: length at
     256 Hz, envelope at 64 Hz, sweep at 128 Hz. */
  apu.seq_period = mc_frames_per_tick(m, 512);
  apu.row_period = mc_frames_per_tick(m, 60) * 6;
  apu.wave_level = 1;
  gb_wave_shape(apu.wave, 0);
  gb_wave_shape(apu.wave_target, 0);
  mc_noise_init(&apu.noise, MC_LFSR_GB_LONG, 0x7F55u);
  mc_noise_set_hz(&apu.noise, apu.rate, ((int32_t)(12000) << 8));
}

static void gb_pulse_note(gb_pulse_t *p, int note, int duty, int vol,
                          int env_period) {
  p->hz_q8 = ((int32_t)(snd_note_hz(note)) << 8);
  mc_osc_set_hz(&p->osc, apu.rate, p->hz_q8);
  p->duty = duty;
  p->vol = vol;
  p->env_period = env_period;
  p->env_acc = 0;
  p->env_dir = -1;
  p->on = 1;
}

static void gb_seq_tick(void) {
  int env = ((apu.seq_step & 7) == 7);  /* 64 Hz  */
  int sweep = ((apu.seq_step & 3) == 2); /* 128 Hz */
  gb_pulse_t *ch[2];
  int i;
  ch[0] = &apu.p1;
  ch[1] = &apu.p2;

  for (i = 0; i < 2; ++i) {
    gb_pulse_t *p = ch[i];
    if (env && p->env_period > 0 && p->on) {
      if (++p->env_acc >= p->env_period) {
        p->env_acc = 0;
        p->vol += p->env_dir;
        if (p->vol <= 0) {
          p->vol = 0;
          p->on = 0;
        }
        if (p->vol > 15)
          p->vol = 15;
      }
    }
  }

  /* Channel 1's sweep unit: the new frequency is derived from the old one by a
     shift, so the slide is geometric rather than linear -- it moves in
     musical intervals, not in hertz. */
  if (sweep && apu.p1.sweep_period > 0 && apu.p1.on) {
    if (++apu.p1.sweep_acc >= apu.p1.sweep_period) {
      apu.p1.sweep_acc = 0;
      apu.p1.hz_q8 += apu.p1.hz_q8 >> apu.p1.sweep_shift;
      if (apu.p1.hz_q8 > ((int32_t)(4000) << 8))
        apu.p1.on = 0;
      mc_osc_set_hz(&apu.p1.osc, apu.rate, apu.p1.hz_q8);
    }
  }

  if (env && apu.noise_vol > 0) {
    if (++apu.noise_env_acc >= 2) {
      apu.noise_env_acc = 0;
      --apu.noise_vol;
    }
  }

  /* Wave RAM morphing: one nibble a step walks toward the target table, so
     the timbre slides instead of switching. A real driver did this from the
     vblank handler with a short unrolled copy. */
  {
    int k = apu.seq_step & 31;
    if (apu.wave[k] < apu.wave_target[k])
      ++apu.wave[k];
    else if (apu.wave[k] > apu.wave_target[k])
      --apu.wave[k];
  }

  ++apu.seq_step;
}

static void gb_row(void) {
  int r = apu.row & 31;
  int n;

  n = gb_lead[r];
  if (n)
    gb_pulse_note(&apu.p1, n, (r & 8) ? 1 : 2, 13, 6);
  n = gb_harm[r];
  if (n)
    gb_pulse_note(&apu.p2, n, 0, 8, 5);
  n = gb_wavebass[r];
  if (n) {
    mc_osc_set_hz(&apu.wave_osc, apu.rate, ((int32_t)(snd_note_hz(n)) << 8));
    apu.wave_on = 1;
  }

  if (gb_drum[r]) {
    apu.noise_short = (gb_drum[r] == 1);
    apu.noise.kind = apu.noise_short ? MC_LFSR_GB_SHORT : MC_LFSR_GB_LONG;
    mc_noise_set_hz(&apu.noise, apu.rate,
                    ((int32_t)((apu.noise_short ? 6000 : 15000)) << 8));
    apu.noise_vol = apu.noise_short ? 7 : 12;
  }

  /* Once a bar, aim wave RAM at a different shape. */
  if (r == 0 || r == 16) {
    apu.wave_shape = (apu.wave_shape + 1) & 3;
    gb_wave_shape(apu.wave_target, apu.wave_shape);
  }

  ++apu.row;
  if (apu.row >= 32)
    apu.row = 0;
}

static int32_t gb_pulse_next(gb_pulse_t *p) {
  if (!p->on)
    return 0;
  MC_OSC_ADVANCE(&p->osc);
  return (mc_wave_pulse(p->osc.phase, gb_duty[p->duty]) * (int32_t)p->vol) /
         15 / 5;
}

static void gb_mix(snd_mixer_t *m, void *user) {
  int i, frames;
  (void)user;
  if (!m)
    return;
  frames = m->block_frames;
  if (frames <= 0)
    return;
  snd_touch_block(m);
  for (i = 0; i < frames; ++i) {
    int32_t acc;
    if (--apu.seq_acc <= 0) {
      apu.seq_acc = apu.seq_period;
      gb_seq_tick();
    }
    if (--apu.row_acc <= 0) {
      apu.row_acc = apu.row_period;
      gb_row();
    }

    acc = gb_pulse_next(&apu.p1) + gb_pulse_next(&apu.p2);

    if (apu.wave_on) {
      /* Step through wave RAM: 32 entries, four bits each, read at the
         channel's frequency times 32. No interpolation anywhere -- the steps
         between nibbles are audible and are most of the character. */
      int32_t sample;
      MC_OSC_ADVANCE(&apu.wave_osc);
      sample = (int)apu.wave[(apu.wave_osc.phase >> 27) & 31] - 8;
      /* Two bits of level: full, half, quarter, mute. */
      switch (apu.wave_level) {
      case 1:
        break;
      case 2:
        sample >>= 1;
        break;
      case 3:
        sample >>= 2;
        break;
      default:
        sample = 0;
        break;
      }
      acc += sample * 380;
    }

    if (apu.noise_vol > 0)
      acc += (mc_noise_next(&apu.noise) * apu.noise_vol) / 15 / 8;
    else
      (void)mc_noise_next(&apu.noise);

    mc_out(m, i, snd_clip_sample(acc), 128);
  }
}

static void gb_sfx(const snd_mixer_t *m, long at_frame) {
  (void)m;
  (void)at_frame;
  /* Channel 1's sweep pointed upward: the coin. */
  gb_pulse_note(&apu.p1, 84, 2, 15, 3);
  apu.p1.sweep_period = 1;
  apu.p1.sweep_shift = 4;
  apu.p1.sweep_acc = 0;
}

/* ---------------------------------------------------------------- */
/* PPU: run once per frame, in tick()                                */
/* ---------------------------------------------------------------- */

/* The per-scanline SCX offset the STAT handler would write. Zero outside the
   band the interrupt is armed over, because an interrupt per line for the
   whole screen is more time than the CPU has. */
static int gb_scx_for_line(int ly) {
  if (ly < GB_RIPPLE_TOP)
    return 0;
  return (mc_sin(gb.ripple + (ly - GB_RIPPLE_TOP) * 22) * 5) >> 15;
}

static void gb_run_ppu(void) {
  int ly, x;
  for (ly = 0; ly < GB_H; ++ly) {
    int scx = gb.scx + gb_scx_for_line(ly);
    int win_row = ly - gb.wy;
    for (x = 0; x < GB_W; ++x) {
      int index;
      /* The window wins wherever it is present. It is not blended and it is
         not clipped by anything: once WX,WY is passed, the background below
         is simply not fetched. */
      if (gb.window_on && win_row >= 0 && x >= gb.wx) {
        int wxp = x - gb.wx;
        int tx = (wxp >> 3) & (GB_MAP - 1);
        int ty = (win_row >> 3) & (GB_MAP - 1);
        index = gb.tiles[gb.win_map[ty * GB_MAP + tx]][(win_row & 7) * 8 +
                                                       (wxp & 7)];
      } else {
        int wx = (x + scx) & (GB_MAP * 8 - 1);
        int wy = (ly + gb.scy) & (GB_MAP * 8 - 1);
        int tx = wx >> 3;
        int ty = wy >> 3;
        index = gb.tiles[gb.bg_map[ty * GB_MAP + tx]][(wy & 7) * 8 + (wx & 7)];
      }
      gb.shade[ly][x] = (uint8_t)gb_bgp_map(index);
    }
  }

  /* The panel. Each pixel chases its target rather than reaching it, which is
     what a 1989 STN cell does. The lag is the reason a DMG can show a smooth
     ripple made of integer pixel steps. */
  for (ly = 0; ly < GB_H; ++ly) {
    for (x = 0; x < GB_W; ++x) {
      int target = gb.shade[ly][x] * 85;
      int cur = gb.lcd[ly][x];
      cur += (target - cur) * 90 / 256;
      gb.lcd[ly][x] = (uint8_t)cur;
    }
  }
}

static void gb_init(int screen_w, int screen_h, const snd_mixer_t *m,
                    long start_frame) {
  (void)screen_w;
  (void)screen_h;
  (void)start_frame;
  memset(&gb, 0, sizeof(gb));
  gb.bgp = 0xE4;  /* 11 10 01 00: the identity mapping */
  gb.wx = 8;
  gb.wy = GB_H;   /* off the bottom, ready to slide up */
  gb.window_on = 1;
  gb.show_debug = 1;
  gb_build_tiles();
  gb_build_maps();
  memset(gb.lcd, 0, sizeof(gb.lcd));
  gb_run_ppu();
  gb_apu_init(m);
}

static void gb_tick(const mr_demo_input_t *input) {
  int phase;
  ++gb.frame;
  gb.scx += 1;
  gb.scy = 24 + ((mc_sin((int)gb.frame * 2) * 8) >> 15);
  gb.ripple = (gb.ripple + 9) & 1023;

  /* WY walked one line per frame: the whole dialogue-box animation. */
  phase = (int)(gb.frame % 400ul);
  if (phase < 100)
    gb.wy = GB_H - phase / 2;
  else if (phase < 240)
    gb.wy = GB_H - 50;
  else if (phase < 340)
    gb.wy = GB_H - 50 + (phase - 240) / 2;
  else
    gb.wy = GB_H;

  /* BGP rotation. Four states, and none of them touch a single byte of tile
     data: fade to white, back, fade to black, back. */
  {
    /* Four of the eight slots are the identity mapping, so the screen spends
       most of its time looking normal and the fade reads as an event. */
    int f = (int)((gb.frame / 45ul) & 7ul);
    static const int ramp[8] = {0xE4, 0xE4, 0xE4, 0xE4,
                                0x90, 0x40, 0x90, 0xE4};
    gb.bgp = ramp[f];
    gb.fade_phase = f;
  }

  if (input) {
    if (input->dx)
      gb.scx += input->dx * 2;
    if ((input->buttons & MR_DEMO_INPUT_DEBUG) != 0u)
      gb.show_debug = !gb.show_debug;
  }

  gb_run_ppu();
}

/* ---------------------------------------------------------------- */
/* present                                                           */
/* ---------------------------------------------------------------- */

static void gb_render(gfx_renderer_t *r) {
  int y, y0, y1;
  gfx_color_t bezel = GFX_RGB565(0x8C, 0x8C, 0x90);
  gfx_color_t bezel_dark = GFX_RGB565(0x40, 0x42, 0x48);
  if (!r)
    return;
  mc_rows(r, &y0, &y1);

  for (y = y0; y < y1; ++y) {
    int x0, tw, x;
    gfx_color_t *row = mc_row(r, y, &x0, &tw);
    int ly = y - GB_Y0;
    if (!row)
      continue;
    for (x = 0; x < tw; ++x)
      row[x] = bezel_dark;
    if (ly < 0 || ly >= GB_H)
      continue;
    for (x = 0; x < GB_W; ++x) {
      int sx = GB_X0 + x;
      int32_t v;
      if (sx < x0 || sx >= x0 + tw)
        continue;
      /* Map the panel's settled level back onto the four shades, blending
         between the two it currently sits between. */
      v = gb.lcd[ly][x];
      {
        int idx = v / 85;
        int frac = (int)((((long)v - (long)idx * 85L) * 256L) / 85L);
        gfx_color_t a = gb_shade[idx > 3 ? 3 : idx];
        gfx_color_t b = gb_shade[idx + 1 > 3 ? 3 : idx + 1];
        row[sx - x0] = mc_rgb_lerp(a, b, frac);
      }
    }
  }

  gfx_draw_rect(r, GB_X0 - 2, GB_Y0 - 2, GB_W + 4, GB_H + 4, bezel);
  mc_text_shadow(r, 8, 8, "GAME BOY - WINDOW LAYER + STAT SCX RIPPLE",
                 GFX_RGB565(200, 210, 190), 1);
  mc_text_shadow(r, 8, 18, "LCD GHOSTING IS SIMULATED, NOT FILTERED",
                 GFX_RGB565(140, 150, 140), 1);

  if (gb.show_debug) {
    char buf[64];
    char *p = buf;
    const char *end = buf + sizeof(buf) - 1;
    int i;
    p = mr_strbuf_str(p, end, "BGP ");
    for (i = 0; i < 4; ++i)
      p = mr_strbuf_u32(p, end, (unsigned long)gb_bgp_map(i));
    p = mr_strbuf_str(p, end, "   WY ");
    p = mr_strbuf_u32(p, end, (unsigned long)gb.wy);
    *p = '\0';
    mc_text_shadow(r, 8, GB_Y0 + GB_H + 8, buf, GFX_RGB565(200, 210, 190), 1);

    /* The shade ramp BGP is currently selecting, drawn as four swatches so the
       register is not merely a number. */
    for (i = 0; i < 4; ++i) {
      gfx_fill_rect(r, 8 + i * 18, GB_Y0 + GB_H + 20, 16, 12,
                    gb_shade[gb_bgp_map(i)]);
      gfx_draw_rect(r, 8 + i * 18, GB_Y0 + GB_H + 20, 16, 12, bezel);
    }
    mc_text_shadow(r, 88, GB_Y0 + GB_H + 22, "4 SHADES, ONE REGISTER",
                   GFX_RGB565(160, 170, 160), 1);
  }
}

/* ---------------------------------------------------------------- */

static const char *const gb_notes[] = {
    "The window is a second tile map, not a sprite layer: WY slides it up.",
    "A STAT interrupt per line rewrites SCX -- the ripple band, not the screen.",
    "BGP remaps four colours to four shades: fades with no VRAM writes.",
    "The panel lags, so motion smears. Games designed around it.",
    "APU channel 3 plays 32 user-written 4-bit samples: a wavetable, in 1989.",
    "Channel 4's shift register drops to 7 bits and becomes tonal.",
    0};

static const mc_example_t gb_example = {
    "gb-window",
    "Nintendo Game Boy DMG (1989), LR35902 PPU + APU",
    "window layer, per-scanline STAT/SCX ripple, BGP fades, simulated LCD ghosting",
    "wave-RAM morphing, hardware envelope and sweep, 7-bit short-LFSR noise",
    gb_notes,
    gb_init,
    gb_tick,
    gb_render,
    gb_mix,
    gb_sfx};

const mc_example_t *mc_example_gb_window(void) { return &gb_example; }
