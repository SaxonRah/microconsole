/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MC_FASTDOOM_AUDIO_H
#define MC_FASTDOOM_AUDIO_H

#include "snd.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MC_FD_AUDIO_RATE
#define MC_FD_AUDIO_RATE 32000
#endif

#ifndef MC_FD_AUDIO_BLOCK
#define MC_FD_AUDIO_BLOCK 256
#endif

#define MC_FD_AUDIO_CHANNELS 2

/* ------------------------------------------------------------------------- */
/* Platform-neutral FastDoom/MicroWave renderer                              */
/* ------------------------------------------------------------------------- */

/* True after FastDoom's S_Init() has initialized the MicroWave Doom core. */
int mc_fd_audio_core_ready(void);

/*
 * Console-wide master volume, applied by MicroWave's final output stage.
 *
 * The value is 16.16 in the same 0..SND_VOL_UNITY range used by snd.h.
 * FastDoom's own music/SFX sliders remain independent upstream controls.
 * A physical potentiometer can therefore drive this without rewriting Doom's
 * per-source volume logic.
 */
void mc_fd_audio_set_master_volume(int32_t volume_16_16);
int32_t mc_fd_audio_master_volume(void);

/*
 * A transport sink receives one finished interleaved stereo MicroWave block:
 *
 *     L0, R0, L1, R1, ...
 *
 * The pointer is valid only for the duration of the call. The sink should
 * immediately convert/copy it into its platform output buffer.
 */
typedef void (*mc_fd_audio_sink_fn)(const snd_sample_t *samples,
                                    unsigned int frames,
                                    void *user);

/*
 * Render exactly `frames` stereo frames and deliver them to `sink`.
 *
 * This is the one authoritative Doom audio clock. Every call advances PCM
 * SFX, MUS scheduling, DMX/GENMIDI state and Nuked OPL by exactly the number
 * of frames requested. The game tick must never advance audio independently.
 *
 * Arbitrary frame counts are accepted. Internally the renderer chunks through
 * MC_FD_AUDIO_BLOCK while holding the audio lock once for the entire request,
 * so a platform callback/DMA refill is one coherent span of audio time.
 */
void mc_fd_audio_render(unsigned int frames,
                        mc_fd_audio_sink_fn sink,
                        void *user);

/* Called by a transport after an unrecoverable device/startup failure. */
void mc_fd_audio_transport_failed(void);

/* ------------------------------------------------------------------------- */
/* Platform transport                                                        */
/* ------------------------------------------------------------------------- */

/*
 * Implement exactly one transport per target.
 *
 * Raylib: AudioStream callback calls mc_fd_audio_render().
 * Pico:   DMA refill calls mc_fd_audio_render().
 */
int  mc_fd_audio_transport_start(void);
int  mc_fd_audio_transport_ready(void);

/*
 * Service transport work that is intentionally kept out of interrupt context.
 * Callback transports such as Raylib implement this as a no-op. The Pico DMA
 * transport uses it to refill the buffer released by the DMA IRQ.
 */
void mc_fd_audio_transport_service(void);

void mc_fd_audio_transport_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* MC_FASTDOOM_AUDIO_H */
