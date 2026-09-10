/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Raylib transport for the platform-neutral FastDoom -> MicroWave renderer.
 *
 * This file owns only the host audio device and format conversion. All Doom
 * audio state and time advancement live in mc_fastdoom_sound_mw.c.
 */

/* Keep Raylib first so none of FastDoom's compatibility macros can affect it. */
#include "raylib.h"
#ifdef true
#undef true
#endif
#ifdef false
#undef false
#endif

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "mc_fastdoom_audio.h"

typedef struct mc_fd_raylib_sink {
    float *out;
    size_t sample_offset;
} mc_fd_raylib_sink_t;

static AudioStream mc_fd_raylib_stream;
static int mc_fd_raylib_ready = 0;
static int mc_fd_raylib_failed = 0;

static void mc_fd_raylib_sink(const snd_sample_t *samples,
                              unsigned int frames,
                              void *user)
{
    mc_fd_raylib_sink_t *sink = (mc_fd_raylib_sink_t *)user;
    long sample_count = (long)frames * MC_FD_AUDIO_CHANNELS;

    snd_pack_float(samples,
                   sink->out + sink->sample_offset,
                   sample_count);
    sink->sample_offset += (size_t)sample_count;
}

static void mc_fd_raylib_callback(void *buffer_data, unsigned int frames)
{
    mc_fd_raylib_sink_t sink;

    if (!buffer_data)
        return;

    sink.out = (float *)buffer_data;
    sink.sample_offset = 0u;
    mc_fd_audio_render(frames, mc_fd_raylib_sink, &sink);
}

int mc_fd_audio_transport_ready(void)
{
    return mc_fd_raylib_ready;
}

void mc_fd_audio_transport_service(void)
{
    /* Raylib owns its refill thread; there is no polling work here. */
}

int mc_fd_audio_transport_start(void)
{
    if (mc_fd_raylib_ready)
        return 1;

    /* Explicit -nosound: there is deliberately no audio core to service. */
    if (!mc_fd_audio_core_ready())
        return 1;

    if (mc_fd_raylib_failed)
        return 0;

    /* FastDoom calls S_Init before I_InitGraphics. Wait until the existing
     * Raylib video frontend has created its window before opening audio. */
    if (!IsWindowReady())
        return 1;

    InitAudioDevice();
    if (!IsAudioDeviceReady())
    {
        fprintf(stderr,
                "MicroWave: Raylib audio device failed to initialize\n");
        mc_fd_raylib_failed = 1;
        mc_fd_audio_transport_failed();
        return 0;
    }

    SetAudioStreamBufferSizeDefault(MC_FD_AUDIO_BLOCK);
    mc_fd_raylib_stream = LoadAudioStream(MC_FD_AUDIO_RATE,
                                          32,
                                          MC_FD_AUDIO_CHANNELS);
    if (!IsAudioStreamValid(mc_fd_raylib_stream))
    {
        fprintf(stderr,
                "MicroWave: Raylib audio stream failed to initialize\n");
        CloseAudioDevice();
        mc_fd_raylib_failed = 1;
        mc_fd_audio_transport_failed();
        return 0;
    }

    SetAudioStreamCallback(mc_fd_raylib_stream, mc_fd_raylib_callback);
    PlayAudioStream(mc_fd_raylib_stream);
    mc_fd_raylib_ready = 1;

    fprintf(stderr,
            "MicroWave FastDoom: %d Hz callback audio, strict Doom/DMX 1.9 "
            "GENMIDI, 9-voice OPL2, freq split 284, Nuked OPL3, "
            "SB-Pro OPL filter, DS* PCM SFX\n",
            MC_FD_AUDIO_RATE);
    fflush(stderr);

    return 1;
}

void mc_fd_audio_transport_stop(void)
{
    if (mc_fd_raylib_ready)
    {
        StopAudioStream(mc_fd_raylib_stream);
        UnloadAudioStream(mc_fd_raylib_stream);
        CloseAudioDevice();
        memset(&mc_fd_raylib_stream, 0, sizeof(mc_fd_raylib_stream));
        mc_fd_raylib_ready = 0;
    }

    mc_fd_raylib_failed = 0;
}
