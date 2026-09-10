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

/* Time spent synthesizing the most recent / slowest 1024-frame DMA period. */
uint32_t mc_fd_audio_pico_last_refill_us(void);
uint32_t mc_fd_audio_pico_max_refill_us(void);

/* Explicit SRAM stack reserved for the dedicated audio core. */
unsigned int mc_fd_audio_pico_core1_stack_bytes(void);

#ifdef __cplusplus
}
#endif

#endif /* MC_FASTDOOM_AUDIO_PICO_H */
