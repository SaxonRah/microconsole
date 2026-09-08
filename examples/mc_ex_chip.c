#include "mc_ex_chip.h"

void mc_osc_reset(mc_osc_t *o) {
  if (!o)
    return;
  o->phase = 0u;
  o->step = 0u;
}

void mc_osc_set_hz(mc_osc_t *o, int rate, int32_t hz_q8) {
  if (!o || rate <= 0)
    return;
  if (hz_q8 < 0)
    hz_q8 = 0;
  /* Phase is 0.32, so one cycle is the full 32-bit range and the step is
     hz * 2^32 / rate. The 8 fractional bits of hz_q8 come out in the wash:
     (hz_q8 / 256) * 2^32 / rate is (hz_q8 << 24) / rate.

     The 64-bit intermediate is not optional -- hz_q8 << 24 overflows int32 for
     anything above about 128 Hz, which is most of the notes anyone plays. */
  o->step = (uint32_t)(((int64_t)hz_q8 << 24) / (int64_t)rate);
}

int32_t mc_wave_pulse(uint32_t phase, uint32_t duty) {
  return ((phase >> 16) < duty) ? 32767 : -32767;
}

int32_t mc_wave_saw(uint32_t phase) {
  return (int32_t)(phase >> 16) - 32768;
}

int32_t mc_wave_tri(uint32_t phase) {
  int32_t p = (int32_t)(phase >> 16); /* 0..65535 */
  if (p < 32768)
    return p * 2 - 32768;
  return 98303 - p * 2;
}

int32_t mc_wave_tri_steps(uint32_t phase, int steps) {
  int32_t v;
  int32_t q;
  if (steps < 2)
    steps = 2;
  v = mc_wave_tri(phase) + 32768; /* 0..65536 */
  q = (v * steps) >> 16;
  if (q >= steps)
    q = steps - 1;
  return (q * 65534) / (steps - 1) - 32767;
}

int32_t mc_wave_sine(uint32_t phase) {
  /* snd_sine_table is a 256-entry full cycle in the mixer's sample range. The
     top 8 bits of the phase index it directly, which is all a period FM chip
     did as well -- Yamaha's sine ROM was 256 entries of a quarter wave. */
  return (int32_t)snd_sine_table[(phase >> 24) & 0xFFu];
}

uint32_t mc_lfsr_step(uint32_t state, int kind) {
  uint32_t bit;
  switch (kind) {
  case MC_LFSR_NES_SHORT:
    bit = ((state ^ (state >> 6)) & 1u);
    return ((state >> 1) | (bit << 14)) & 0x7FFFu;
  case MC_LFSR_GB_SHORT:
    bit = ((state ^ (state >> 1)) & 1u);
    return ((state >> 1) | (bit << 6)) & 0x7Fu;
  case MC_LFSR_GB_LONG:
    bit = ((state ^ (state >> 1)) & 1u);
    return ((state >> 1) | (bit << 14)) & 0x7FFFu;
  case MC_LFSR_SN76489:
    /* The SN76489 XORs two taps of a 16-bit register; the Master System's
       variant taps bits 0 and 3. Feeding one bit back into bit 15 gives the
       32767-step sequence that is the hiss on every Sega 8-bit title. */
    bit = ((state ^ (state >> 3)) & 1u);
    return ((state >> 1) | (bit << 15)) & 0xFFFFu;
  case MC_LFSR_NES_LONG:
  default:
    bit = ((state ^ (state >> 1)) & 1u);
    return ((state >> 1) | (bit << 14)) & 0x7FFFu;
  }
}

int32_t mc_lfsr_out(uint32_t state) { return (state & 1u) ? 32767 : -32767; }

void mc_noise_init(mc_noise_t *n, int kind, uint32_t seed) {
  if (!n)
    return;
  n->kind = kind;
  n->state = seed ? seed : 1u;
  n->acc = 0u;
  n->step = 0u;
  n->level = mc_lfsr_out(n->state);
}

void mc_noise_set_hz(mc_noise_t *n, int rate, int32_t hz_q8) {
  if (!n || rate <= 0)
    return;
  if (hz_q8 < 0)
    hz_q8 = 0;
  n->step = (uint32_t)(((int64_t)hz_q8 << 8) / (int64_t)rate);
}

int32_t mc_noise_next(mc_noise_t *n) {
  if (!n)
    return 0;
  n->acc += n->step;
  while (n->acc >= 0x10000u) {
    n->acc -= 0x10000u;
    n->state = mc_lfsr_step(n->state, n->kind);
    n->level = mc_lfsr_out(n->state);
  }
  return n->level;
}

void mc_svf_reset(mc_svf_t *f) {
  if (!f)
    return;
  f->low = 0;
  f->band = 0;
}

int32_t mc_svf_cutoff(int rate, int hz) {
  int32_t f;
  if (rate <= 0)
    return 0;
  /* 2*sin(pi*fc/rate) linearized as 2*pi*fc/rate, which is within a few
     percent below rate/8 and is where these filters are actually swept. */
  f = (int32_t)(((int64_t)hz * 411775LL) / (int64_t)rate); /* 2*pi<<16 */
  if (f > 0x18000)
    f = 0x18000; /* keep the two integrators stable */
  if (f < 0x0100)
    f = 0x0100;
  return f;
}

int32_t mc_svf(mc_svf_t *f, int32_t in, int32_t cut_q16, int32_t res_q16,
               int mode) {
  int32_t high, out;
  if (!f)
    return in;
  high = in - f->low - (int32_t)(((int64_t)f->band * res_q16) >> 16);
  f->band += (int32_t)(((int64_t)high * cut_q16) >> 16);
  f->low += (int32_t)(((int64_t)f->band * cut_q16) >> 16);
  switch (mode) {
  case MC_SVF_HIGH:
    out = high;
    break;
  case MC_SVF_BAND:
    out = f->band;
    break;
  case MC_SVF_LOW:
  default:
    out = f->low;
    break;
  }
  /* Resonance can overshoot well past full scale; clamp the state as well as
     the output so a swept filter cannot latch into a permanent howl. */
  if (f->low > 200000)
    f->low = 200000;
  if (f->low < -200000)
    f->low = -200000;
  if (f->band > 200000)
    f->band = 200000;
  if (f->band < -200000)
    f->band = -200000;
  return snd_clip_sample(out);
}

int32_t mc_lp1(mc_lp1_t *f, int32_t in, int32_t k_q16) {
  if (!f)
    return in;
  f->z += (int32_t)((((int64_t)in - f->z) * k_q16) >> 16);
  return snd_clip_sample(f->z);
}

void mc_delay_init(mc_delay_t *d, snd_sample_t *buf, long frames) {
  if (!d)
    return;
  d->buf = buf;
  d->len = frames > 0 ? frames : 0;
  d->pos = 0;
  mc_delay_clear(d);
}

void mc_delay_clear(mc_delay_t *d) {
  long i;
  if (!d || !d->buf)
    return;
  for (i = 0; i < d->len; ++i)
    d->buf[i] = SND_SAMPLE_SILENCE;
}

int32_t mc_delay_read(const mc_delay_t *d, long frames_back) {
  long i;
  if (!d || !d->buf || d->len <= 0)
    return 0;
  if (frames_back < 0)
    frames_back = 0;
  if (frames_back >= d->len)
    frames_back = d->len - 1;
  i = d->pos - frames_back;
  while (i < 0)
    i += d->len;
  return SND_SAMPLE_TO_MIX(d->buf[i]);
}

void mc_delay_write(mc_delay_t *d, int32_t value) {
  if (!d || !d->buf || d->len <= 0)
    return;
  d->buf[d->pos] = (snd_sample_t)SND_MIX_TO_SAMPLE(snd_clip_sample(value));
}

void mc_delay_advance(mc_delay_t *d) {
  if (!d || d->len <= 0)
    return;
  ++d->pos;
  if (d->pos >= d->len)
    d->pos = 0;
}

int32_t mc_crush(int32_t value, int bits) {
  int shift;
  if (bits >= 16 || bits <= 0)
    return value;
  shift = 16 - bits;
  /* Arithmetic shift on a negative value is implementation-defined in C99, so
     bias into unsigned, truncate, and come back. Every compiler these targets
     use does the obvious thing, but "every compiler I tried" is not the same
     claim as "defined". */
  return (int32_t)((((uint32_t)(value + 32768) >> shift) << shift)) - 32768;
}

int32_t mc_psg_attenuate(int32_t value, int step) {
  /* 2 dB per step is a factor of about 0.794. Sixteen entries of 8.8 gain,
     with 15 hard silent, is the whole SN76489 volume register. */
  static const int16_t att[16] = {256, 203, 161, 128, 102, 81, 64, 51,
                                  40,  32,  26,  20,  16,  13, 10, 0};
  if (step < 0)
    step = 0;
  if (step > 15)
    step = 15;
  return (value * (int32_t)att[step]) >> 8;
}

void mc_out(snd_mixer_t *m, int f, int32_t value, int pan_q8) {
  if (!m)
    return;
  if (m->channels >= 2) {
    int r = pan_q8;
    int l;
    if (r < 0)
      r = 0;
    if (r > 256)
      r = 256;
    l = 256 - r;
    snd_block_add(m, (long)f * 2L, ((long)value * l) >> 8);
    snd_block_add(m, (long)f * 2L + 1L, ((long)value * r) >> 8);
  } else {
    snd_block_add(m, (long)f, value);
  }
}

long mc_frames_per_tick(const snd_mixer_t *m, int hz) {
  long n;
  if (!m || hz <= 0)
    return 1;
  n = (long)m->rate / (long)hz;
  return n > 0 ? n : 1;
}
