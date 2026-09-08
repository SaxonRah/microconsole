/* Sega Mega Drive / Genesis, 1988 -- VDP 315-5313 + YM2612 + SN76489
 *
 * Display: scroll tables, and a mode that changes brightness rather than
 * colour.
 *
 *   The Master System scrolled its one plane with one register. The Mega Drive
 *   has two planes and, more importantly, does not scroll them with a register
 *   at all: it reads a *table* out of VRAM. The horizontal scroll table holds
 *   one entry per scanline for plane A and one for plane B, and the VDP fetches
 *   the pair as it starts each line. Nobody has to interrupt anything.
 *
 *   That is the difference between the NES's raster split and this. On the NES
 *   a mid-frame scroll change costs a sprite-0 poll or a cartridge IRQ and the
 *   CPU has to be there. Here the CPU writes 224 values into a table once and
 *   goes away, and every scanline gets its own offset. Curved parallax,
 *   heat haze, the water surface in Sonic -- all of them are a table with a
 *   sine in it, and none of them cost anything per line.
 *
 *   Vertical is the mirror image and less well known: VSRAM holds a vertical
 *   scroll value per *two-cell column*, sixteen pixels wide. So the plane can
 *   shear vertically in columns while it scrolls horizontally in lines. Used
 *   together, as they are here, one background plane bends in both axes.
 *
 *   Shadow and highlight is the other headline mode. A tile's priority bit
 *   stops meaning "in front" and starts meaning "at full brightness"; low
 *   priority regions render at half. Two specific palette entries flip it the
 *   other way and brighten to roughly one and a half. It is not alpha and it
 *   does not blend two layers -- it multiplies one. Which is exactly why it
 *   was affordable, and why Mega Drive shadows are a uniform darkening rather
 *   than a colour.
 *
 *   And the thing the machine is unfairly remembered for: it cannot blend, so
 *   transparency is a checkerboard. A one-pixel dither of water over rock
 *   through an RF modulator and a composite decoder came out as a genuine
 *   50% mix, because the encoder's colour bandwidth was far below one pixel.
 *   The waterfall below is a real checkerboard; the COMPOSITE toggle applies
 *   what a TV of the period would have done to it. On an RGB monitor -- or an
 *   emulator -- you get the checkerboard, which is why so much Mega Drive
 *   footage looks harsher now than it did then.
 *
 * Audio: six FM channels, one of which is not.
 *
 *   The YM2612 is four-operator FM. This example uses two operators per voice,
 *   which is what most Mega Drive instruments effectively were once the
 *   envelopes settled: a modulator whose output is added to the carrier's
 *   phase. Change the modulator's level over time and the timbre changes with
 *   it, which is the thing subtractive chips could not do at all -- there is
 *   no filter here and none is needed.
 *
 *   Feedback on the modulator is the other half. An operator fed its own last
 *   output turns progressively from a sine into a saw and then into noise, and
 *   the feedback level is a per-channel register, so the grit is programmable.
 *
 *   Channel 6 is special: setting one bit turns it from an FM voice into an
 *   8-bit DAC that the CPU writes samples to, one byte at a time, in an
 *   interrupt. That is where Mega Drive drums come from, and it explains their
 *   character completely -- they are 8-bit, usually around 8 to 16 kHz, and
 *   they cost a whole FM channel plus most of the CPU's spare time. Six voices
 *   became five the moment you wanted a snare.
 *
 *   The SN76489 from the Master System is still on the die, and Mega Drive
 *   music kept using it for hi-hats, because a PSG noise channel costs nothing
 *   and the FM channels were all spoken for.
 */

#include "mc_ex_chip.h"
#include "mc_ex_gfx.h"
#include "mc_example.h"
#include "mr_strbuf.h"

#include <string.h>

#define MD_W 320
#define MD_H 224 /* the VDP's 320x224 NTSC display */
#define MD_Y0 8
#define MD_MAP 64
#define MD_TILES 16
#define MD_WATER 178 /* screen row the water surface starts on */

static struct {
  unsigned long frame;
  int scroll_a, scroll_b;
  int composite; /* simulate what a period TV did to the dither */
  int show_debug;

  /* The scroll tables themselves. One entry per line for each plane, and one
     vertical entry per two-cell column, exactly as the VDP reads them. */
  int16_t hscroll_a[MC_EX_H];
  int16_t hscroll_b[MC_EX_H];
  int16_t vscroll_a[MD_W / 16 + 1];

  uint8_t tiles[MD_TILES][8 * 8];
  uint8_t plane_a[MD_MAP * 32];
  uint8_t plane_b[MD_MAP * 32];
  uint8_t prio_a[MD_MAP * 32]; /* the priority bit, reinterpreted by S/H */
  gfx_color_t pal[4][16];
} md;

/* RGB333: nine bits, 512 colours, and four palettes of sixteen on screen. Note
   the quantization -- three bits a channel is coarser than the SNES's five and
   is why Mega Drive gradients band where SNES ones do not. */
static gfx_color_t md_color(int r, int g, int b) {
  return mc_rgb_quant(GFX_RGB565(r, g, b), 3, 3, 3);
}

static void md_build_palettes(void) {
  int i;
  /* 0: sky and far mountains */
  md.pal[0][0] = md_color(40, 60, 140);
  for (i = 1; i < 8; ++i)
    md.pal[0][i] = md_color(40 + i * 14, 60 + i * 12, 140 + i * 14);
  md.pal[0][8] = md_color(70, 60, 110);
  md.pal[0][9] = md_color(90, 80, 130);
  md.pal[0][10] = md_color(120, 110, 160);
  md.pal[0][11] = md_color(200, 210, 240);
  md.pal[0][12] = md_color(30, 40, 90);
  md.pal[0][13] = md_color(255, 240, 200);
  md.pal[0][14] = md_color(180, 190, 220);
  md.pal[0][15] = md_color(10, 12, 30);
  /* 1: ground */
  md.pal[1][0] = md_color(0, 0, 0);
  md.pal[1][1] = md_color(70, 130, 50);
  md.pal[1][2] = md_color(50, 100, 40);
  md.pal[1][3] = md_color(120, 90, 50);
  md.pal[1][4] = md_color(90, 70, 40);
  md.pal[1][5] = md_color(160, 140, 90);
  md.pal[1][6] = md_color(200, 190, 140);
  md.pal[1][7] = md_color(40, 60, 30);
  for (i = 8; i < 16; ++i)
    md.pal[1][i] = md_color(30 + i * 8, 25 + i * 6, 20 + i * 4);
  /* 2: water */
  for (i = 0; i < 16; ++i)
    md.pal[2][i] = md_color(20 + i * 6, 70 + i * 9, 150 + i * 6);
  /* 3: highlight/accent */
  for (i = 0; i < 16; ++i)
    md.pal[3][i] = md_color(200 + i * 3, 180 + i * 4, 60 + i * 10);
}

static void md_build_tiles(void) {
  int t, x, y;
  for (t = 0; t < MD_TILES; ++t) {
    for (y = 0; y < 8; ++y) {
      for (x = 0; x < 8; ++x) {
        int v = 0;
        switch (t) {
        case 0:
          v = 0;
          break;
        case 1: /* sky */
          v = 3;
          break;
        case 2: /* cloud */
          v = ((x - 4) * (x - 4) + (y - 4) * (y - 4) < 15) ? 11 : 0;
          break;
        case 3: /* far mountain body */
          v = 9;
          break;
        case 4: /* mountain slope, right-facing */
          v = (y >= 7 - x) ? 9 : 3;
          break;
        case 12: /* mountain slope, left-facing */
          v = (y >= x) ? 9 : 3;
          break;
        case 13: /* snow cap */
          v = (y >= 7 - x && y >= x) ? 11 : 3;
          break;
        case 5: /* grass top */
          v = (y == 0) ? 1 : ((y < 3) ? 2 : 7);
          break;
        case 6: /* rock */
          v = (((x * 5 + y * 3) & 7) < 3) ? 4 : 3;
          break;
        case 7: /* sand */
          v = (((x * 3 + y * 7) & 15) < 3) ? 6 : 5;
          break;
        case 8: /* water surface */
          v = (y < 2) ? 14 : ((((x + y) & 3) < 2) ? 10 : 8);
          break;
        case 9: /* water body */
          v = (((x * 3 + y) & 7) < 3) ? 6 : 4;
          break;
        case 10: /* pillar */
          v = (x < 2 || x > 5) ? 3 : 5;
          break;
        default:
          v = (t + x + y) & 15;
          break;
        }
        md.tiles[t][y * 8 + x] = (uint8_t)v;
      }
    }
  }
}

static void md_build_planes(void) {
  int x, y;
  uint32_t rng = 0x1357u;
  for (y = 0; y < 32; ++y) {
    for (x = 0; x < MD_MAP; ++x) {
      int b, a, prio;
      /* Plane B: sky, clouds, distant mountains. */
      {
        /* A ridge line: peaks every eleven cells, drawn with two slope tiles
           and a cap. Plane B is the only thing behind the ground, so this is
           the whole of "distance" on this machine. */
        int col = x % 11;
        int peak_row = 19 + ((col < 5) ? col : (10 - col)) / 2;
        if (y > peak_row + 1)
          b = 3;
        else if (y == peak_row + 1)
          b = (col < 5) ? 4 : 12;
        else if (y == peak_row && col == 5)
          b = 13;
        else if ((mc_rand(&rng) & 31u) < 2u && y < 14)
          b = 2;
        else
          b = 1;
      }
      md.plane_b[y * MD_MAP + x] = (uint8_t)b;

      /* Plane A: the ground and the water below it. */
      prio = 1;
      if (y >= 24)
        a = 9;
      else if (y == 23)
        a = 8;
      else if (y >= 20)
        a = 6;
      else if (y == 19)
        a = 5;
      else if ((x % 11) == 4 && y >= 15 && y < 19)
        a = 10;
      else
        a = 0;
      /* Below the water surface the priority bit is cleared, which under
         shadow/highlight means "render this at half brightness". */
      if (y >= 23)
        prio = 0;
      md.plane_a[y * MD_MAP + x] = (uint8_t)a;
      md.prio_a[y * MD_MAP + x] = (uint8_t)prio;
    }
  }
}

/* ---------------------------------------------------------------- */
/* YM2612 (two operators per voice) + DAC + PSG                      */
/* ---------------------------------------------------------------- */

#define MD_FM_VOICES 5 /* six channels, minus the one the DAC took */
#define MD_DAC_LEN 3000

typedef struct fm_voice {
  mc_osc_t car;
  mc_osc_t mod;
  int mod_ratio;   /* modulator frequency as a multiple of the carrier's */
  int mod_index;   /* how hard the modulator bends the carrier's phase */
  int feedback;    /* 0..7, as on the chip */
  int32_t fb_z1;
  int32_t fb_z2;
  snd_env_t car_env;
  snd_env_t mod_env; /* a separate envelope on the modulator: the timbre moves */
  long start;
  int16_t gain;
  int active;
} fm_voice_t;

static struct {
  int rate;
  long frame;
  long row_acc;
  long row_period;
  int row;

  fm_voice_t v[MD_FM_VOICES];
  int next_voice;

  /* Channel 6 in DAC mode. */
  int8_t dac[MD_DAC_LEN];
  long dac_len;
  uint32_t dac_pos;
  uint32_t dac_step;
  int dac_on;

  /* The SN76489 is still here, doing hats. */
  mc_noise_t psg_noise;
  int psg_atten;
  long psg_acc;
} ym;

/* Bake an 8-bit drum for the DAC. Deliberately quantized to eight bits and
   played back at 11 kHz, because that is the budget a 68000 had left after
   running a game -- and the resulting grit is the sound of the machine. */
static void md_bake_dac(void) {
  long i;
  uint32_t rng = 0xC0DEu;
  int32_t amp = 32767;
  mc_osc_t body;
  mc_lp1_t lp;
  lp.z = 0;
  mc_osc_reset(&body);
  mc_osc_set_hz(&body, 11025, ((int32_t)(150) << 8));
  ym.dac_len = MD_DAC_LEN;
  for (i = 0; i < ym.dac_len; ++i) {
    int32_t noise = (int32_t)(mc_rand(&rng) & 0xFFFFu) - 32768;
    int32_t v;
    MC_OSC_ADVANCE(&body);
    v = (mc_wave_sine(body.phase) >> 1) + (mc_lp1(&lp, noise, 20000) >> 1);
    v = (int)(((long)snd_clip_sample(v) * amp) >> 15);
    /* Eight bits, signed: the whole dynamic range the DAC register has. */
    ym.dac[i] = (int8_t)(v >> 8);
    amp -= amp >> 8;
    body.step -= body.step >> 10; /* the pitch drop that makes it a kick */
  }
}

static void md_fm_key_on(int note, int ratio, int index, int feedback,
                         int16_t gain, long ma, long md_, int16_t ms, long mr,
                         long ca, long cd, int16_t cs, long cr) {
  fm_voice_t *v = &ym.v[ym.next_voice];
  int hz = snd_note_hz(note);
  ym.next_voice = (ym.next_voice + 1) % MD_FM_VOICES;
  mc_osc_set_hz(&v->car, ym.rate, ((int32_t)(hz) << 8));
  mc_osc_set_hz(&v->mod, ym.rate, ((int32_t)((hz * ratio)) << 8));
  v->car.phase = 0u;
  v->mod.phase = 0u;
  v->mod_ratio = ratio;
  v->mod_index = index;
  v->feedback = feedback;
  v->fb_z1 = 0;
  v->fb_z2 = 0;
  v->gain = gain;
  v->start = ym.frame;
  v->active = 1;
  snd_env_init(&v->mod_env, ma, md_, ms, mr);
  snd_env_init(&v->car_env, ca, cd, cs, cr);
}

static int32_t md_fm_next(fm_voice_t *v) {
  long since;
  int16_t mlevel, clevel;
  int32_t mod_out, car_out;
  int32_t fb;
  if (!v->active)
    return 0;
  since = ym.frame - v->start;
  mlevel = snd_env_level(&v->mod_env, since, -1);
  clevel = snd_env_level(&v->car_env, since, -1);
  if (clevel <= 0 && since > 400) {
    v->active = 0;
    return 0;
  }

  /* Feedback: the modulator is phase-modulated by the average of its own last
     two outputs. Averaging two samples rather than using one is what the chip
     does, and it is what keeps high feedback settings from turning into pure
     noise immediately. */
  fb = (v->fb_z1 + v->fb_z2) >> 1;
  MC_OSC_ADVANCE(&v->mod);
  mod_out = mc_wave_sine(v->mod.phase +
                         (uint32_t)((fb * (long)v->feedback) >> 4));
  v->fb_z2 = v->fb_z1;
  v->fb_z1 = mod_out;
  mod_out = (int)(((long)mod_out * mlevel) >> 8);

  MC_OSC_ADVANCE(&v->car);
  car_out = mc_wave_sine(
      v->car.phase + (uint32_t)(((long)mod_out * v->mod_index) >> 6));
  car_out = (int)(((long)car_out * clevel) >> 8);
  return (int)(((long)car_out * v->gain) >> 8);
}

static const uint8_t md_lead[32] = {76, 0,  79, 0,  83, 0,  0,  81,
                                    0,  79, 0,  0,  76, 0,  0,  0,
                                    74, 0,  77, 0,  81, 0,  0,  79,
                                    0,  77, 0,  0,  74, 0,  0,  0};
static const uint8_t md_bass[32] = {40, 0, 40, 0, 47, 0, 40, 0,
                                    45, 0, 45, 0, 52, 0, 45, 0,
                                    38, 0, 38, 0, 45, 0, 38, 0,
                                    43, 0, 43, 0, 50, 0, 50, 0};
static const uint8_t md_stab[32] = {0, 0, 0, 0, 64, 0, 0, 0, 0, 0, 0,
                                    0, 62, 0, 0, 0, 0, 0, 0, 0, 59, 0,
                                    0, 0, 0,  0, 0, 0, 57, 0, 0, 0};
static const uint8_t md_dac[32] = {1, 0, 0, 0, 2, 0, 0, 1, 1, 0, 0,
                                   0, 2, 0, 0, 0, 1, 0, 0, 0, 2, 0,
                                   1, 0, 1, 0, 0, 1, 2, 0, 2, 0};
static const uint8_t md_hat[32] = {0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1,
                                   0, 0, 0, 1, 1, 0, 0, 1, 0, 0, 0,
                                   1, 0, 0, 0, 1, 0, 0, 1, 1, 1};

static void md_audio_init(const snd_mixer_t *m) {
  memset(&ym, 0, sizeof(ym));
  ym.rate = m ? m->rate : SND_DEFAULT_RATE;
  ym.row_period = mc_frames_per_tick(m, 60) * 6;
  md_bake_dac();
  /* The CPU feeds the DAC from a timer interrupt. 11 kHz was a common choice:
     fast enough for a snare, slow enough to leave the 68000 some cycles. */
  ym.dac_step = (uint32_t)(((int64_t)11025 << 16) / (int64_t)ym.rate);
  ym.psg_atten = 15;
  mc_noise_init(&ym.psg_noise, MC_LFSR_SN76489, 0x8000u);
  mc_noise_set_hz(&ym.psg_noise, ym.rate, ((int32_t)(14000) << 8));
}

static void md_row(void) {
  int r = ym.row & 31;
  int n;

  n = md_lead[r];
  if (n)
    /* Ratio 2, moderate index, envelope on the modulator falling faster than
       the carrier's: bright on the attack, mellow on the tail. That single
       relationship is most of what "an FM lead" means. */
    md_fm_key_on(n, 2, 40, 4, (int16_t)(SND_GAIN_UNITY / 4), 20, 2500,
                 (int16_t)(SND_GAIN_UNITY / 8), 2000, 40, 6000,
                 (int16_t)(SND_GAIN_UNITY / 2), 4000);
  n = md_bass[r];
  if (n)
    /* Ratio 1 with heavy feedback: the classic Mega Drive slap bass, which is
       an operator being driven until it stops being a sine. */
    md_fm_key_on(n, 1, 90, 7, (int16_t)(SND_GAIN_UNITY / 3), 5, 1200,
                 (int16_t)(SND_GAIN_UNITY / 10), 800, 10, 4000,
                 (int16_t)(SND_GAIN_UNITY / 3), 1500);
  n = md_stab[r];
  if (n)
    md_fm_key_on(n, 3, 28, 2, (int16_t)(SND_GAIN_UNITY / 6), 200, 4000,
                 (int16_t)(SND_GAIN_UNITY / 6), 3000, 300, 8000,
                 (int16_t)(SND_GAIN_UNITY / 4), 6000);

  if (md_dac[r]) {
    ym.dac_pos = 0u;
    ym.dac_on = 1;
  }
  if (md_hat[r])
    ym.psg_atten = 6;

  ++ym.row;
  if (ym.row >= 32)
    ym.row = 0;
}

static void md_mix(snd_mixer_t *m, void *user) {
  int i, frames, v;
  (void)user;
  if (!m)
    return;
  frames = m->block_frames;
  if (frames <= 0)
    return;
  snd_touch_block(m);
  for (i = 0; i < frames; ++i) {
    int32_t acc = 0;
    if (--ym.row_acc <= 0) {
      ym.row_acc = ym.row_period;
      md_row();
    }
    for (v = 0; v < MD_FM_VOICES; ++v)
      acc += md_fm_next(&ym.v[v]);

    if (ym.dac_on) {
      long idx = (long)(ym.dac_pos >> 16);
      if (idx >= ym.dac_len) {
        ym.dac_on = 0;
      } else {
        /* Eight bits, held between writes. No interpolation: the steps are
           the sound. */
        acc += (int32_t)ym.dac[idx] * 80;
        ym.dac_pos += ym.dac_step;
      }
    }

    if (ym.psg_atten < 15) {
      acc += mc_psg_attenuate(mc_noise_next(&ym.psg_noise) / 8, ym.psg_atten);
      if (--ym.psg_acc <= 0) {
        ym.psg_acc = mc_frames_per_tick(m, 300);
        ++ym.psg_atten;
      }
    } else {
      (void)mc_noise_next(&ym.psg_noise);
    }

    mc_out(m, i, snd_clip_sample(acc), 128);
    ++ym.frame;
  }
}

static void md_sfx(const snd_mixer_t *m, long at_frame) {
  (void)m;
  (void)at_frame;
  md_fm_key_on(88, 5, 70, 6, (int16_t)(SND_GAIN_UNITY / 3), 2, 900,
               (int16_t)(SND_GAIN_UNITY / 12), 600, 5, 3000,
               (int16_t)(SND_GAIN_UNITY / 4), 2000);
}

/* ---------------------------------------------------------------- */
/* simulation                                                        */
/* ---------------------------------------------------------------- */

static void md_build_scroll_tables(void) {
  int y, c;
  for (y = 0; y < MC_EX_H; ++y) {
    int ly = y - MD_Y0;
    int a, b;
    if (ly < 0)
      ly = 0;
    /* Plane B: distance parallax. Rows nearer the horizon move less, which is
       simply a different table entry rather than a different layer. */
    b = md.scroll_b / 4 + (ly * md.scroll_b) / 900;
    /* Plane A: the ground moves at full speed, and below the water surface
       every line gets a sine offset. This is the Sonic water effect, and it
       is a table, not an interrupt. */
    a = md.scroll_a;
    if (ly >= MD_WATER)
      a += (mc_sin((int)md.frame * 8 + (ly - MD_WATER) * 26) * 7) >> 15;
    md.hscroll_a[y] = (int16_t)a;
    md.hscroll_b[y] = (int16_t)b;
  }
  /* VSRAM: one vertical scroll per two-cell column, so the plane can shear
     vertically while it scrolls horizontally. */
  for (c = 0; c <= MD_W / 16; ++c)
    md.vscroll_a[c] = (int16_t)((mc_sin((int)md.frame * 5 + c * 60) * 6) >> 15);
}

static void md_init(int screen_w, int screen_h, const snd_mixer_t *m,
                    long start_frame) {
  (void)screen_w;
  (void)screen_h;
  (void)start_frame;
  memset(&md, 0, sizeof(md));
  md.composite = 1;
  md.show_debug = 1;
  md_build_palettes();
  md_build_tiles();
  md_build_planes();
  md_build_scroll_tables();
  md_audio_init(m);
}

static void md_tick(const mr_demo_input_t *input) {
  ++md.frame;
  md.scroll_a -= 3;
  md.scroll_b -= 1;
  if (input) {
    if (input->dx)
      md.scroll_a -= input->dx * 3;
    if ((input->buttons & MR_DEMO_INPUT_ACTION) != 0u)
      md.composite = !md.composite;
    if ((input->buttons & MR_DEMO_INPUT_DEBUG) != 0u)
      md.show_debug = !md.show_debug;
  }
  md_build_scroll_tables();
}

/* ---------------------------------------------------------------- */
/* render                                                            */
/* ---------------------------------------------------------------- */

static void md_render(gfx_renderer_t *r) {
  int y, y0, y1;
  if (!r)
    return;
  mc_rows(r, &y0, &y1);

  for (y = y0; y < y1; ++y) {
    int x0, tw, x;
    gfx_color_t *row = mc_row(r, y, &x0, &tw);
    int ly = y - MD_Y0;
    int ha, hb;
    if (!row)
      continue;
    for (x = 0; x < tw; ++x)
      row[x] = md.pal[0][15];
    if (ly < 0 || ly >= MD_H)
      continue;

    ha = md.hscroll_a[y];
    hb = md.hscroll_b[y];

    for (x = 0; x < tw; ++x) {
      int sx = x0 + x;
      int va = md.vscroll_a[(sx >> 4) <= MD_W / 16 ? (sx >> 4) : MD_W / 16];
      int bx = ((sx - hb) & (MD_MAP * 8 - 1));
      int by = (ly + 62) & 255;
      int ax = ((sx - ha) & (MD_MAP * 8 - 1));
      int ay = (ly + va + 6) & 255;
      int tile_b, tile_a, prio, index;
      gfx_color_t c;

      tile_b = md.plane_b[(by >> 3) * MD_MAP + (bx >> 3)];
      index = md.tiles[tile_b][(by & 7) * 8 + (bx & 7)];
      c = md.pal[0][index];

      tile_a = md.plane_a[(ay >> 3) * MD_MAP + (ax >> 3)];
      prio = md.prio_a[(ay >> 3) * MD_MAP + (ax >> 3)];
      if (tile_a) {
        index = md.tiles[tile_a][(ay & 7) * 8 + (ax & 7)];
        if (index) {
          int bank = (tile_a >= 8) ? 2 : 1;
          c = md.pal[bank][index];
          /* Shadow/highlight. The priority bit stops meaning depth and starts
             meaning brightness: cleared is half, set is full, and two
             reserved palette entries push to about one and a half. */
          if (!prio)
            c = mc_rgb_scale(c, 1, 2);
          else if (index == 14)
            c = mc_rgb_scale(c, 3, 2);
        }
      }
      row[x] = c;
    }

    /* A dithered waterfall. Every other pixel, alternating with the row, which
       is the only transparency the VDP has. */
    if (ly > 40 && ly < MD_H) {
      int wx = 200 + ((mc_sin((int)md.frame * 6 + ly * 30) * 5) >> 15);
      int px;
      for (px = wx; px < wx + 40; ++px) {
        if (px < x0 || px >= x0 + tw)
          continue;
        if (((px + ly + (int)(md.frame & 1ul)) & 1) == 0)
          continue;
        row[px - x0] = md.pal[2][11];
      }
      /* What a composite encoder did to that checkerboard: its colour
         bandwidth is far below one pixel, so adjacent pixels average. This is
         a two-tap horizontal blend, which is roughly what came out of an RF
         modulator in 1990. */
      if (md.composite) {
        gfx_color_t prev = row[0];
        for (px = wx - 2; px < wx + 42; ++px) {
          int i2 = px - x0;
          if (i2 < 1 || i2 >= tw)
            continue;
          {
            gfx_color_t cur = row[i2];
            row[i2] = mc_rgb_half(cur, prev);
            prev = cur;
          }
        }
      }
    }
  }

  mc_text_shadow(r, 8, 12, "MEGA DRIVE - PER-LINE SCROLL TABLE, NOT AN IRQ",
                 md.pal[0][11], 1);
  if (md.show_debug) {
    char buf[64];
    char *p = buf;
    const char *end = buf + sizeof(buf) - 1;
    p = mr_strbuf_str(p, end, "HSCROLL ENTRIES ");
    p = mr_strbuf_u32(p, end, (unsigned long)MD_H);
    p = mr_strbuf_str(p, end, "  VSRAM COLS ");
    p = mr_strbuf_u32(p, end, (unsigned long)(MD_W / 16));
    *p = '\0';
    mc_text_shadow(r, 8, 22, buf, md.pal[0][14], 1);
    mc_text_shadow(r, 8, MD_WATER + 6, "PRIORITY BIT CLEAR HERE = SHADOW",
                   md.pal[0][11], 1);
    mc_text_shadow(r, 8, MC_EX_H - 14,
                   md.composite ? "WATERFALL: DITHER + COMPOSITE BLEND"
                                : "WATERFALL: RAW DITHER (RGB MONITOR)",
                   md.pal[3][12], 1);
  }
}

/* ---------------------------------------------------------------- */

static const char *const md_notes[] = {
    "H-scroll is a table in VRAM, one entry per line, per plane.",
    "The CPU writes it once a frame. No interrupt, no sprite-0 poll.",
    "VSRAM holds a vertical scroll per 16-pixel column: the plane shears.",
    "Shadow/highlight makes the priority bit mean brightness, not depth.",
    "Transparency is a checkerboard; a composite encoder made it a blend.",
    "YM2612: 2-op FM with a moving modulator envelope and feedback.",
    "Channel 6 becomes an 8-bit DAC for drums, costing an FM voice.",
    0};

static const mc_example_t md_example = {
    "md-linescroll",
    "Sega Mega Drive / Genesis (1988), VDP 315-5313 + YM2612 + SN76489",
    "per-line H-scroll table, per-column VSRAM shear, shadow/highlight, dither transparency",
    "2-operator FM with feedback, channel-6 8-bit DAC drums, PSG hats",
    md_notes,
    md_init,
    md_tick,
    md_render,
    md_mix,
    md_sfx};

const mc_example_t *mc_example_md_linescroll(void) { return &md_example; }
