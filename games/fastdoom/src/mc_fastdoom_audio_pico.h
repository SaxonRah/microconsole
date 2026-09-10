/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MC_FASTDOOM_AUDIO_PICO_H
#define MC_FASTDOOM_AUDIO_PICO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Number of DMA periods that had to repeat because a refill was not ready. */
unsigned long mc_fd_audio_pico_underruns(void);

/* Number of audio periods synthesized into a DMA buffer. */
unsigned long mc_fd_audio_pico_refills(void);

/* Non-zero once the dedicated RP2350 core-1 audio producer is alive. */
int mc_fd_audio_pico_core1_running(void);

/* Time spent synthesizing one complete transport period. */
uint32_t mc_fd_audio_pico_last_refill_us(void);
uint32_t mc_fd_audio_pico_min_refill_us(void);
uint32_t mc_fd_audio_pico_avg_refill_us(void);
uint32_t mc_fd_audio_pico_max_refill_us(void);

unsigned int mc_fd_audio_pico_block_frames(void);
unsigned int mc_fd_audio_pico_period_us(void);
unsigned int mc_fd_audio_pico_ring_ready(void);
unsigned int mc_fd_audio_pico_ring_count(void);
unsigned int mc_fd_audio_pico_ring_target(void);
unsigned int mc_fd_audio_pico_ring_low_water(void);
unsigned int mc_fd_audio_pico_ring_high_water(void);

/* Explicit SRAM stack reserved for the dedicated audio core. */
unsigned int mc_fd_audio_pico_core1_stack_bytes(void);
unsigned int mc_fd_audio_pico_core1_stack_used_bytes(void);

/*
 * Execute one rare control callback on the dedicated audio core, at a safe
 * boundary between 256-frame MicroWave render chunks.
 *
 * This is deliberately synchronous for Doom's song lifecycle: Core 0 keeps
 * ownership of WAD/zone memory, posts the already-cached song pointer, and
 * waits while Core 1 alone mutates MUS/MIDI/GENMIDI/Nuked state.
 */
typedef void (*mc_fd_audio_pico_control_fn)(void *user);
int mc_fd_audio_pico_run_control(mc_fd_audio_pico_control_fn fn,
                                 void *user,
                                 unsigned int timeout_us);

/* Diagnostics for transition-time failures. */
unsigned long mc_fd_audio_pico_control_timeouts(void);
int mc_fd_audio_pico_control_paused(void);

#ifdef __cplusplus
}
#endif

#endif /* MC_FASTDOOM_AUDIO_PICO_H */
