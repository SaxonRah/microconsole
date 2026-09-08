#ifndef MC_EX_CHIP_H
#define MC_EX_CHIP_H

#include "snd.h"
#include "snd_synth.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sound-chip building blocks.
 *
 * MicroWave gives every example a mix block, a span, an envelope, a note
 * table and a sine table. What it deliberately does not give anyone is a
 * pulse-width register, a 15-bit LFSR with a Sega tap, an FM operator, or an
 * echo buffer -- those are one chip's opinions, and snd_synth.h exists to be
 * the opinion-free floor underneath them.
 *
 * So each example emulates its own chip out of these pieces and writes the
 * result through snd_touch_block() / snd_block_add(), which is the documented
 * route for "your own generator". None of it reaches into m->block, and all of
 * it survives someone enabling the wide accumulator.
 *
 * Everything is integer and seeded. Two builds at one mix rate produce
 * identical samples, for the reason mw_music_demo.c gives: a difference
 * between targets should be a yes-or-no question.
 */

/* ---------------------------------------------------------------- */
/* oscillators                                                       */
/* ---------------------------------------------------------------- */

/* Phase is 0.32: the whole 32-bit range is exactly one cycle, so it wraps for
   free on overflow and the inner loop is one add with no comparison.

   Every waveform below reads the top bits of it and nothing else -- a pulse
   takes 16, the sine table takes 8, a 32-entry wavetable takes 5. That is the
   only reason they can share one accumulator, so a generator that wants to
   advance its own counter at a rate should keep a separate 16.16 accumulator
   rather than borrowing this step. */
typedef struct mc_osc {
  uint32_t phase;
  uint32_t step;
} mc_osc_t;

/* hz_q8 is frequency in 8.8 fixed point, because a vibrato or a pulse-width
   sweep that can only land on whole hertz audibly staircases in the octave
   where these chips actually sang. */
void mc_osc_set_hz(mc_osc_t *o, int rate, int32_t hz_q8);
void mc_osc_reset(mc_osc_t *o);

#define MC_OSC_ADVANCE(o) ((o)->phase += (o)->step)

/* All oscillators return roughly -32767..32767, the mixer's own sample range,
   so a caller scales by an 8.8 gain and shifts by 8 exactly as the voice
   mixers do.

   Every one of these returns int32_t rather than int, for the reason
   mc_sin() does: on a 16-bit-int target a full-scale sample already fills the
   type, and the gain multiply that always follows would wrap. The values are
   the same on both; only the promotion rules differ. */
int32_t mc_wave_pulse(uint32_t phase, uint32_t duty); /* duty 0..65535 */
int32_t mc_wave_saw(uint32_t phase);
int32_t mc_wave_tri(uint32_t phase);
/* Triangle quantized to `steps` levels. The 2A03's triangle channel walks a
   4-bit counter, so it is a 16-step staircase and not a triangle at all --
   which is most of why NES bass has that particular hollow buzz. */
int32_t mc_wave_tri_steps(uint32_t phase, int steps);
int32_t mc_wave_sine(uint32_t phase); /* reads MicroWave's snd_sine_table */

/* ---------------------------------------------------------------- */
/* noise                                                             */
/* ---------------------------------------------------------------- */

/* Linear feedback shift registers, tapped the way the real parts were.
   Returns the new state; bit 0 is the output bit.

   MC_LFSR_NES_LONG   15-bit, taps 0 and 1     -- 2A03 normal noise
   MC_LFSR_NES_SHORT  15-bit, taps 0 and 6     -- 2A03 "periodic" noise, 93
                                                  steps, tonal rather than hissy
   MC_LFSR_GB_LONG    15-bit, taps 0 and 1     -- Game Boy normal
   MC_LFSR_GB_SHORT    7-bit, taps 0 and 1     -- Game Boy short mode, the
                                                  metallic one
   MC_LFSR_SN76489    16-bit, parallel taps    -- SN76489 white noise */
#define MC_LFSR_NES_LONG 0
#define MC_LFSR_NES_SHORT 1
#define MC_LFSR_GB_LONG 2
#define MC_LFSR_GB_SHORT 3
#define MC_LFSR_SN76489 4

uint32_t mc_lfsr_step(uint32_t state, int kind);
/* Output level for the current state, +/- full scale. */
int32_t mc_lfsr_out(uint32_t state);

/* A noise generator clocked at its own rate rather than the mix rate, which is
   what makes a period noise channel pitched: the shift register only advances
   every N output frames, and N is the register the composer wrote to. */
typedef struct mc_noise {
  uint32_t state;
  uint32_t acc;  /* 16.16 accumulator against the shift period */
  uint32_t step; /* 16.16 shifts per output frame */
  int kind;
  int32_t level;
} mc_noise_t;

void mc_noise_init(mc_noise_t *n, int kind, uint32_t seed);
void mc_noise_set_hz(mc_noise_t *n, int rate, int32_t hz_q8);
int32_t mc_noise_next(mc_noise_t *n);

/* ---------------------------------------------------------------- */
/* filters                                                           */
/* ---------------------------------------------------------------- */

/* Chamberlin state-variable filter, integer. Two integrators and a feedback
   term: the same topology the 6581's on-chip filter implements in analogue,
   which is why a resonant sweep through it sounds like a SID and not like a
   one-pole rolloff. */
#define MC_SVF_LOW 0
#define MC_SVF_BAND 1
#define MC_SVF_HIGH 2

typedef struct mc_svf {
  int32_t low;
  int32_t band;
} mc_svf_t;

void mc_svf_reset(mc_svf_t *f);
/* cut_q16 is 2*sin(pi*fc/rate) in 16.16 and is clamped to stay stable;
   res_q16 is 1/Q, so smaller means more resonance. */
int32_t mc_svf(mc_svf_t *f, int32_t in, int32_t cut_q16, int32_t res_q16,
               int mode);
/* Convenience: turn a cutoff in Hz into the coefficient above, integer only. */
int32_t mc_svf_cutoff(int rate, int hz);

/* One-pole lowpass, for the places a gentle rolloff is all that is wanted --
   a DAC reconstruction filter, or the deliberate dullness of an amplifier at
   the end of a console's audio path. */
typedef struct mc_lp1 {
  int32_t z;
} mc_lp1_t;
int32_t mc_lp1(mc_lp1_t *f, int32_t in, int32_t k_q16);

/* ---------------------------------------------------------------- */
/* delay line                                                        */
/* ---------------------------------------------------------------- */

/* Caller owns the buffer, because MicroWave owns nothing and neither should
   this. Length is in frames. */
typedef struct mc_delay {
  snd_sample_t *buf;
  long len;
  long pos;
} mc_delay_t;

void mc_delay_init(mc_delay_t *d, snd_sample_t *buf, long frames);
void mc_delay_clear(mc_delay_t *d);
int32_t mc_delay_read(const mc_delay_t *d, long frames_back);
void mc_delay_write(mc_delay_t *d, int32_t value);
void mc_delay_advance(mc_delay_t *d);

/* ---------------------------------------------------------------- */
/* quantization                                                      */
/* ---------------------------------------------------------------- */

/* Round a sample to `bits` of resolution. Every chip here is coarser than the
   mixer: the SN76489 has 4-bit logarithmic volume, the Game Boy's wave RAM is
   4-bit, a Mega Drive DAC drum is 8-bit. Crushing on the way out is not an
   effect, it is the actual signal path. */
int32_t mc_crush(int32_t value, int bits);

/* 4-bit logarithmic attenuator, 2 dB per step, silent at 15. The SN76489's
   volume register exactly; the reason PSG fades sound like they are falling
   off a staircase. */
int32_t mc_psg_attenuate(int32_t value, int step);

/* ---------------------------------------------------------------- */
/* helpers for writing generated audio into the block                */
/* ---------------------------------------------------------------- */

/* Add one mono value to every channel of frame `f` of the current block,
   panned when the mixer has two channels. pan_q8 is 0 hard left, 128 centre,
   256 hard right; ignored on a mono mixer.

   Every frontend MicroConsole ships runs the mixer mono today, so panning is
   written but not heard there. It is here anyway because two of these chips --
   Paula and the Mega Drive's YM2612 -- had stereo as a headline feature, and
   an example that silently drops it would be teaching the wrong history. */
void mc_out(snd_mixer_t *m, int f, int32_t value, int pan_q8);

/* Frames per 1/60 s tick at the mixer's rate, which is the clock nearly every
   one of these chips ran its envelopes and effects off: the vertical blank. */
long mc_frames_per_tick(const snd_mixer_t *m, int hz);

#ifdef __cplusplus
}
#endif

#endif /* MC_EX_CHIP_H */
