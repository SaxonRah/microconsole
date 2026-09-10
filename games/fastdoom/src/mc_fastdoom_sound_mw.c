/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * FastDoom -> MicroWave sound core for MicroConsole.
 *
 * Ownership is intentionally split at the same place as the original Doom
 * sound layer:
 *
 *   FastDoom owns
 *     - music/SFX requests
 *     - WAD lump selection and cache lifetime
 *     - channel priority and spatial attenuation
 *     - menu music/SFX volume and stereo separation
 *
 *   MicroWave owns
 *     - MUS timing
 *     - MIDI state
 *     - strict Doom/DMX 1.9 GENMIDI -> OPL register generation
 *     - Nuked OPL3 running as OPL2
 *     - Doom DS* PCM mixing
 *     - the SB-Pro-like OPL output filter
 *     - the transport-driven audio clock and final output mix
 *
 * This replaces the deliberately silent first-bring-up stub. It does not use
 * FastDoom's DOS AdLib/Multivoc runtime implementation.
 *
 * Platform transport is deliberately outside this file. Raylib calls
 * mc_fd_audio_render() from its AudioStream callback; Pico calls the same
 * function when a DMA buffer needs refilling. That keeps the Doom/MicroWave
 * clock, MUS state, OPL synthesis, filtering and SFX mixing identical.
 */

/*
 * Keep host/system headers before FastDoom headers. FastDoom intentionally
 * defines several CRT-like macros, so including host headers later can poison
 * their declarations. The core does not include Raylib, Pico SDK, or any
 * transport API: platform audio lives in a separate transport module.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <stdatomic.h>
#endif

#include "doomdef.h"
#include "doomstat.h"
#include "fastmath.h"
#include "i_sound.h"
#include "m_misc.h"
#include "p_local.h"
#include "s_sound.h"
#include "sounds.h"
#include "w_wad.h"
#include "z_zone.h"

#include "snd.h"
#include "snd_genmidi_opl.h"
#include "snd_midi.h"
#include "snd_mus.h"

#include "mc_fastdoom_audio.h"

#if defined(MC_FASTDOOM_PICO) && defined(__GNUC__)
#define MC_FD_AUDIO_HOT(name) \
    __attribute__((noinline, section(".time_critical." #name))) name
#else
#define MC_FD_AUDIO_HOT(name) name
#endif

#ifndef MC_FD_OPL_POST_GAIN_SHIFT
#define MC_FD_OPL_POST_GAIN_SHIFT 0
#endif


#if defined(MC_FASTDOOM_PICO)
#include "mc_fastdoom_audio_pico.h"
#endif

#ifndef MC_FD_AUDIO_RATE
#define MC_FD_AUDIO_RATE 32000
#endif

#ifndef MC_FD_AUDIO_BLOCK
#define MC_FD_AUDIO_BLOCK 256
#endif

#define MC_FD_OPL_CHANNELS 1
#define MC_FD_MUSIC_MAX 255
#define MC_FD_SFX_MAX S_MAX_VOLUME
#define MC_FD_DMX_TYPE_PCM 3u
#define MC_FD_DMX_HEADER_BYTES 8u
#define MC_FD_DMX_PAD_BYTES 16u
#define MC_FD_DMX_PCM_OFFSET (MC_FD_DMX_HEADER_BYTES + MC_FD_DMX_PAD_BYTES)
#define MC_FD_DMX_TOTAL_PAD_BYTES (MC_FD_DMX_PAD_BYTES * 2u)
#define MC_FD_FIRST_SFX_HANDLE 1
#define MC_FD_LAST_SFX_HANDLE 30000

/* First-order OPL output coloration. At the fixed 32 kHz MicroConsole rate,
 * 47988/65536 gives a one-pole LPF corner very close to 8 kHz. The 0.995 DC
 * blocker pole is about 25 Hz at 32 kHz. Keep this on the OPL bus only: PCM
 * SFX retain FastDoom's stereo separation and are mixed after the filter. */
#define MC_FD_DC_R_Q16 65208L
#define MC_FD_LP_A_Q16 47988L

/* FastDoom platform/config globals normally supplied by i_sound.c. */
int snd_Mport = 0x330;
int snd_Sport = 0x378;
int snd_Rate = 7;      /* FastDoom's legacy 32000-Hz selector. */
int snd_PCMRate = 1;
int snd_GoldIrq = 0;
int snd_GoldDma = 0;

int snd_MusicVolume = 0;
int snd_SfxVolume = 0;

int snd_SfxDevice = snd_SB;
int snd_MusicDevice = snd_Adlib;
int snd_MidiDevice = midi_default;

int snd_DesiredSfxDevice = snd_SB;
int snd_DesiredMusicDevice = snd_Adlib;
int snd_DesiredMidiDevice = midi_default;

int snd_clipping = S_CLIPPING_DIST;

extern int numChannels;

/* ------------------------------------------------------------------------- */
/* FastDoom-side channel bookkeeping                                         */
/* ------------------------------------------------------------------------- */

typedef struct mc_fd_channel {
    sfxinfo_t *sfxinfo;
    void *origin;
    int handle;
} mc_fd_channel_t;

static mc_fd_channel_t *mc_channels = NULL;
static byte mc_music_paused = 0;
static musicinfo_t *mc_music_playing = NULL;

/* ------------------------------------------------------------------------- */
/* Audio-thread-owned MicroWave state                                        */
/* ------------------------------------------------------------------------- */

static snd_mixer_t mc_mix;
static snd_mixer_t mc_opl_mix;
static snd_sample_t mc_mix_block[MC_FD_AUDIO_BLOCK * MC_FD_AUDIO_CHANNELS];
static snd_sample_t mc_opl_block[MC_FD_AUDIO_BLOCK * MC_FD_OPL_CHANNELS];
#if SND_WIDE_ACCUM
static int32_t mc_mix_accum[MC_FD_AUDIO_BLOCK * MC_FD_AUDIO_CHANNELS];
#endif

static snd_bank_t mc_sfx_bank;
static snd_clip_t mc_sfx_clips[NUMSFX];
static byte mc_sfx_clip_valid[NUMSFX];

static snd_genmidi_opl_bank_t mc_genmidi_bank;
static snd_genmidi_opl_t mc_opl;
static snd_midi_t mc_midi;
static snd_mus_song_t mc_song;
static snd_mus_player_t mc_player;

static long mc_audio_frame = 0;
static long mc_music_frame = 0;
static int mc_core_ready = 0;
static int mc_genmidi_ready = 0;
static int mc_atexit_registered = 0;
static int mc_music_active = 0;
static int mc_music_backend_paused = 0;
static unsigned long mc_music_messages_emitted = 0ul;
static int mc_music_opl_peak = 0;
static int mc_music_opl_peak_max = 0;
static int mc_music_filtered_peak = 0;
static int mc_music_filtered_peak_max = 0;
static unsigned long mc_music_post_gain_clips = 0ul;
static int mc_next_sfx_handle = MC_FD_FIRST_SFX_HANDLE;
static int mc_audio_error = 0;
static int mc_audio_error_reported = 0;
static int32_t mc_master_volume = SND_VOL_UNITY;

static int32_t mc_dc_x1 = 0;
static int32_t mc_dc_y1 = 0;
static int32_t mc_lp_y1 = 0;


static void mc_sound_shutdown(void);

#if defined(_MSC_VER)
static volatile long mc_audio_lock = 0;
static int mc_audio_lock_ready = 0;

static void mc_lock_init(void)
{
    mc_audio_lock = 0;
    mc_audio_lock_ready = 1;
}

static void mc_lock(void)
{
    if (!mc_audio_lock_ready)
        return;

    while (_InterlockedCompareExchange(&mc_audio_lock, 1, 0) != 0)
    {
#if defined(_M_IX86) || defined(_M_X64)
        _mm_pause();
#endif
    }
}

static void mc_unlock(void)
{
    if (mc_audio_lock_ready)
        (void)_InterlockedExchange(&mc_audio_lock, 0);
}

static void mc_lock_destroy(void)
{
    mc_audio_lock_ready = 0;
    mc_audio_lock = 0;
}
#else
static atomic_flag mc_audio_lock = ATOMIC_FLAG_INIT;
static int mc_audio_lock_ready = 0;

static void mc_lock_init(void)
{
    atomic_flag_clear_explicit(&mc_audio_lock, memory_order_release);
    mc_audio_lock_ready = 1;
}

static void mc_lock(void)
{
    if (!mc_audio_lock_ready)
        return;
    while (atomic_flag_test_and_set_explicit(&mc_audio_lock,
                                              memory_order_acquire))
    {
    }
}

static void mc_unlock(void)
{
    if (mc_audio_lock_ready)
        atomic_flag_clear_explicit(&mc_audio_lock, memory_order_release);
}

static void mc_lock_destroy(void)
{
    mc_audio_lock_ready = 0;
}
#endif

/* ------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* ------------------------------------------------------------------------- */

static uint32_t mc_rd32le(const byte *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int mc_clampi(int value, int lo, int hi)
{
    if (value < lo)
        return lo;
    if (value > hi)
        return hi;
    return value;
}

static int16_t mc_music_gain_8_8(int volume)
{
    long v = mc_clampi(volume, 0, MC_FD_MUSIC_MAX);
    return (int16_t)((v * 256L + 127L) / 255L);
}

/* Multivoc's stereo path picks MV_VolumeTable[volume >> 2], whose maximum
 * useful index is 63. Preserve that quantized Doom/DMX gain law instead of
 * replacing it with a modern constant-power pan. */
static int16_t mc_dmx_sfx_gain_8_8(int volume)
{
    long index;

    volume = mc_clampi(volume, 0, 255);
    index = (long)(volume >> 2);
    return (int16_t)((index * 256L) / 63L);
}

static void mc_filter_reset(void)
{
    mc_dc_x1 = 0;
    mc_dc_y1 = 0;
    mc_lp_y1 = 0;
}

static long MC_FD_AUDIO_HOT(mc_filter_opl)(long sample)
{
    int32_t x = (int32_t)sample;
    int32_t hp;
    int32_t lp;

    hp = x - mc_dc_x1 +
         (int32_t)(((int64_t)MC_FD_DC_R_Q16 * (int64_t)mc_dc_y1) >> 16);
    mc_dc_x1 = x;
    mc_dc_y1 = hp;

    lp = mc_lp_y1 +
         (int32_t)(((int64_t)MC_FD_LP_A_Q16 *
                    (int64_t)(hp - mc_lp_y1)) >> 16);
    mc_lp_y1 = lp;

    if (lp > SND_MIX_MAX || lp < SND_MIX_MIN)
        ++mc_music_post_gain_clips;

    return (long)snd_clip_sample((long)lp);
}

static int mc_prepare_sfx_clip(int sfx_index, void *vdata)
{
    byte *data;
    uint32_t declared_bytes;
    uint32_t pcm_bytes;
    int lump_bytes;
    snd_clip_t *clip;

    if (sfx_index <= 0 || sfx_index >= NUMSFX || !vdata)
        return 0;

    if (mc_sfx_clip_valid[sfx_index])
        return 1;

    lump_bytes = W_LumpLength(S_sfx[sfx_index].lumpnum);
    if (lump_bytes < (int)MC_FD_DMX_PCM_OFFSET)
        return 0;

    data = (byte *)vdata;
    if ((((unsigned int)data[1] << 8) | (unsigned int)data[0]) !=
        MC_FD_DMX_TYPE_PCM)
        return 0;

    declared_bytes = mc_rd32le(data + 4);

    /* FastDoom's DMX path does exactly this for type-3 Doom sounds:
     *
     *   data = lump + 8-byte header
     *   skip 16 leading pad bytes
     *   remove 16 trailing pad bytes
     *
     * hence lump+24 and declared_length-32. */
    if (declared_bytes <= 48u)
        return 0;
    if ((uint64_t)MC_FD_DMX_HEADER_BYTES + (uint64_t)declared_bytes >
        (uint64_t)(unsigned int)lump_bytes)
        return 0;

    pcm_bytes = declared_bytes - MC_FD_DMX_TOTAL_PAD_BYTES;

    clip = &mc_sfx_clips[sfx_index];
    memset(clip, 0, sizeof(*clip));
    clip->data = data + MC_FD_DMX_PCM_OFFSET;
    clip->bytes = pcm_bytes;
    clip->frames = pcm_bytes;
    clip->rate = (int)((unsigned int)data[2] |
                       ((unsigned int)data[3] << 8));
    clip->channels = 1u;
    clip->flags = SND_CLIP_PCM8;

    if (!snd_clip_validate(clip))
    {
        memset(clip, 0, sizeof(*clip));
        return 0;
    }

    mc_sfx_clip_valid[sfx_index] = 1;
    return 1;
}

static int mc_load_genmidi(void)
{
    int lump;
    int bytes;
    void *data;
    int error;

    lump = W_GetNumForName("GENMIDI");
    bytes = W_LumpLength(lump);
    data = W_CacheLumpNum(lump, PU_CACHE);
    if (!data || bytes <= 0)
        return 0;

    error = snd_genmidi_opl_load_bank(&mc_genmidi_bank,
                                      data,
                                      (uint32_t)(unsigned int)bytes);
    if (error != SND_GENMIDI_OPL_OK)
    {
        fprintf(stderr, "MicroWave: GENMIDI load failed: %s\n",
                snd_genmidi_opl_error_string(error));
        return 0;
    }

    return 1;
}

static void mc_core_init(int want_music)
{
    if (mc_core_ready)
        return;

    mc_lock_init();

    memset(mc_sfx_clips, 0, sizeof(mc_sfx_clips));
    memset(mc_sfx_clip_valid, 0, sizeof(mc_sfx_clip_valid));

    snd_init(&mc_mix,
             MC_FD_AUDIO_RATE,
             MC_FD_AUDIO_CHANNELS,
             mc_mix_block,
             MC_FD_AUDIO_BLOCK,
             NULL,
             NULL);
    snd_set_volume_ramp(&mc_mix, MC_FD_AUDIO_RATE / 50); /* 20 ms */
    snd_set_master_volume_now(&mc_mix, mc_master_volume);
#if SND_WIDE_ACCUM
    snd_set_accumulator(&mc_mix, mc_mix_accum);
#endif
    snd_init(&mc_opl_mix,
             MC_FD_AUDIO_RATE,
             MC_FD_OPL_CHANNELS,
             mc_opl_block,
             MC_FD_AUDIO_BLOCK,
             NULL,
             NULL);

    snd_bank_init(&mc_sfx_bank);
    snd_midi_init(&mc_midi);
    snd_genmidi_opl_init(&mc_opl, MC_FD_AUDIO_RATE);
    snd_genmidi_opl_set_freq_model(&mc_opl,
                                   SND_GENMIDI_OPL_FREQ_DOOM19_284);
    snd_genmidi_opl_bind(&mc_opl, &mc_midi);

    mc_genmidi_ready = want_music ? mc_load_genmidi() : 0;
    if (mc_genmidi_ready)
        snd_genmidi_opl_set_bank(&mc_opl, &mc_genmidi_bank);

    mc_audio_frame = 0;
    mc_music_frame = 0;
    mc_music_active = 0;
    mc_music_backend_paused = 0;
    mc_next_sfx_handle = MC_FD_FIRST_SFX_HANDLE;
    mc_audio_error = 0;
    mc_audio_error_reported = 0;
    mc_filter_reset();
    mc_core_ready = 1;

    if (!mc_atexit_registered)
    {
        if (atexit(mc_sound_shutdown) == 0)
            mc_atexit_registered = 1;
    }
}

static void mc_set_music_volume_backend(int volume)
{
    if (!mc_core_ready)
        return;

    mc_lock();
    snd_genmidi_opl_set_output_gain(&mc_opl,
                                    mc_music_gain_8_8(volume));
    mc_unlock();
}

static void mc_stop_music_backend_locked(void)
{
    if (mc_music_active)
    {
        snd_mus_player_stop(&mc_player);
        snd_genmidi_opl_all_sound_off(&mc_opl);
    }

    mc_music_active = 0;
    mc_music_backend_paused = 0;
    mc_filter_reset();
}

typedef enum mc_music_control_op
{
    MC_MUSIC_CONTROL_NONE = 0,
    MC_MUSIC_CONTROL_STOP,
    MC_MUSIC_CONTROL_START
} mc_music_control_op_t;

typedef struct mc_music_control_request
{
    mc_music_control_op_t op;
    void *data;
    int bytes;
    int looping;
    int result;
    int error;
} mc_music_control_request_t;

/*
 * Persistent storage is intentional.  If a pathological Core-1 failure causes
 * the synchronous mailbox wait to time out, a late Core-1 completion still
 * sees valid request memory rather than a dead Core-0 stack frame.
 */
static mc_music_control_request_t mc_music_control_request;

static void mc_music_control_execute(void *user)
{
    mc_music_control_request_t *request =
        (mc_music_control_request_t *)user;
    int error;

    if (!request)
        return;

    request->result = 0;
    request->error = 0;

    mc_lock();

    if (request->op == MC_MUSIC_CONTROL_STOP)
    {
        mc_stop_music_backend_locked();
        request->result = 1;
        mc_unlock();
        return;
    }

    if (request->op != MC_MUSIC_CONTROL_START ||
        !request->data ||
        request->bytes <= 0)
    {
        mc_unlock();
        return;
    }

    /*
     * Core 1 owns the complete synth transition:
     *
     *   old MUS stop -> all voices off -> parse new MUS
     *   -> reset MIDI -> reset/reinitialize Nuked/GENMIDI
     *   -> bind bank/channel state -> start player
     *
     * This is the same full OPL reset sequence used by the earlier
     * single-core/desktop implementation where INTROA and E1M5 were known to
     * work.  The difference is that it now executes on the same core that calls
     * OPL3_GenerateResampled(), never concurrently from Core 0.
     */
    mc_stop_music_backend_locked();

    error = snd_mus_open(&mc_song,
                         request->data,
                         (uint32_t)(unsigned int)request->bytes);
    if (error != SND_MUS_OK)
    {
        request->error = error;
        mc_unlock();
        return;
    }

    snd_midi_reset(&mc_midi);
    snd_genmidi_opl_reset(&mc_opl);
    snd_genmidi_opl_set_bank(&mc_opl, &mc_genmidi_bank);
    snd_genmidi_opl_set_freq_model(&mc_opl,
                                   SND_GENMIDI_OPL_FREQ_DOOM19_284);
    snd_genmidi_opl_bind(&mc_opl, &mc_midi);
    snd_genmidi_opl_set_output_gain(&mc_opl,
                                    mc_music_gain_8_8(snd_MusicVolume));

    /*
     * One authoritative transport clock.  A new song begins at the current
     * audio frame rather than inheriting a music-only clock that stopped while
     * no music was active.
     */
    mc_music_frame = mc_audio_frame;

    mc_music_messages_emitted = 0ul;
    mc_music_opl_peak = 0;
    mc_music_opl_peak_max = 0;
    mc_music_filtered_peak = 0;
    mc_music_filtered_peak_max = 0;
    mc_music_post_gain_clips = 0ul;

    if (!snd_mus_player_init(&mc_player,
                             &mc_song,
                             MC_FD_AUDIO_RATE,
                             SND_MUS_DOOM_TICK_HZ,
                             mc_music_frame,
                             request->looping ? 1 : 0))
    {
        request->error = SND_MUS_ERR_ARGUMENT;
        mc_unlock();
        return;
    }

    mc_music_active = 1;
    mc_music_backend_paused = 0;
    mc_audio_error = 0;
    mc_audio_error_reported = 0;
    mc_filter_reset();

    request->result = 1;
    mc_unlock();
}

static int mc_run_music_control(mc_music_control_op_t op,
                                void *data,
                                int bytes,
                                int looping)
{
    int dispatched;

    if (!mc_core_ready)
        return op == MC_MUSIC_CONTROL_STOP ? 1 : 0;

    if (op == MC_MUSIC_CONTROL_START &&
        (!mc_genmidi_ready || !data || bytes <= 0))
        return 0;

    mc_music_control_request.op = op;
    mc_music_control_request.data = data;
    mc_music_control_request.bytes = bytes;
    mc_music_control_request.looping = looping;
    mc_music_control_request.result = 0;
    mc_music_control_request.error = 0;

#if defined(MC_FASTDOOM_PICO)
    dispatched = mc_fd_audio_pico_run_control(
        mc_music_control_execute,
        &mc_music_control_request,
        500000u);
#else
    mc_music_control_execute(&mc_music_control_request);
    dispatched = 1;
#endif

    if (!dispatched)
    {
        fprintf(stderr,
                "MicroWave: Core 1 music-control transaction timed out\n");
        fflush(stderr);
        return 0;
    }

    if (!mc_music_control_request.result &&
        mc_music_control_request.error != 0)
    {
        fprintf(stderr,
                "MicroWave: music backend failed: %s\n",
                snd_mus_error_string(mc_music_control_request.error));
        fflush(stderr);
    }

    return mc_music_control_request.result;
}

static int mc_stop_music_backend(void)
{
    return mc_run_music_control(MC_MUSIC_CONTROL_STOP, NULL, 0, 0);
}

static int mc_start_music_backend(void *data, int bytes, int looping)
{
    return mc_run_music_control(MC_MUSIC_CONTROL_START,
                                data,
                                bytes,
                                looping);
}

static void mc_pause_music_backend(void)
{
    if (!mc_core_ready)
        return;

    mc_lock();
    if (mc_music_active)
    {
        mc_music_backend_paused = 1;
        mc_filter_reset();
    }
    mc_unlock();
}

static void mc_resume_music_backend(void)
{
    if (!mc_core_ready)
        return;

    mc_lock();
    if (mc_music_active)
        mc_music_backend_paused = 0;
    mc_unlock();
}

static int mc_start_sfx_backend(int sfx_index, void *vdata, int sep, int vol)
{
    snd_voice_t *voice;
    int handle;
    int left;
    int right;
    int16_t gain_l;
    int16_t gain_r;

    if (!mc_core_ready || !vdata)
        return -1;

    if (!mc_prepare_sfx_clip(sfx_index, vdata))
        return -1;

    sep = mc_clampi(sep, 0, 254);
    vol = mc_clampi(vol, 0, MC_FD_SFX_MAX);

    /* Exact FastDoom/DMX stereo parameters before Multivoc's volume-table
     * quantization. */
    left = Div63((254 - sep) * vol);
    right = Div63(sep * vol);

    gain_l = mc_dmx_sfx_gain_8_8(left);
    gain_r = mc_dmx_sfx_gain_8_8(right);

    if (reverseStereo)
    {
        int16_t tmp = gain_l;
        gain_l = gain_r;
        gain_r = tmp;
    }

    mc_lock();

    voice = snd_bank_alloc(&mc_sfx_bank);
    if (!voice)
    {
        mc_unlock();
        return -1;
    }

    handle = mc_next_sfx_handle++;
    if (mc_next_sfx_handle > MC_FD_LAST_SFX_HANDLE)
        mc_next_sfx_handle = MC_FD_FIRST_SFX_HANDLE;

    snd_voice_start(voice,
                    &mc_sfx_clips[sfx_index],
                    &mc_mix,
                    mc_audio_frame,
                    256,
                    256);
    voice->gain_l = gain_l;
    voice->gain_r = gain_r;
    voice->id = (int16_t)handle;

    mc_unlock();
    return handle;
}

static void mc_stop_sfx_backend(int handle)
{
    snd_voice_t *voice;

    if (!mc_core_ready || handle <= 0)
        return;

    mc_lock();
    voice = snd_bank_find(&mc_sfx_bank, (int16_t)handle);
    if (voice)
        snd_voice_stop(voice);
    mc_unlock();
}

static int mc_sfx_playing_backend(int handle)
{
    snd_voice_t *voice;
    int playing;

    if (!mc_core_ready || !mc_fd_audio_transport_ready() || handle <= 0)
        return 0;

    mc_lock();
    voice = snd_bank_find(&mc_sfx_bank, (int16_t)handle);
    playing = (voice && voice->active) ? 1 : 0;
    mc_unlock();
    return playing;
}

static void mc_update_sfx_backend(int handle, int sep, int vol)
{
    snd_voice_t *voice;
    int left;
    int right;
    int16_t gain_l;
    int16_t gain_r;

    if (!mc_core_ready || handle <= 0)
        return;

    sep = mc_clampi(sep, 0, 254);
    vol = mc_clampi(vol, 0, MC_FD_SFX_MAX);
    left = Div63((254 - sep) * vol);
    right = Div63(sep * vol);
    gain_l = mc_dmx_sfx_gain_8_8(left);
    gain_r = mc_dmx_sfx_gain_8_8(right);

    if (reverseStereo)
    {
        int16_t tmp = gain_l;
        gain_l = gain_r;
        gain_r = tmp;
    }

    mc_lock();
    voice = snd_bank_find(&mc_sfx_bank, (int16_t)handle);
    if (voice)
    {
        voice->gain_l = gain_l;
        voice->gain_r = gain_r;
    }
    mc_unlock();
}

static void mc_invalidate_sfx_clip(int index)
{
    if (index <= 0 || index >= NUMSFX)
        return;
    mc_lock();
    mc_sfx_clip_valid[index] = 0;
    memset(&mc_sfx_clips[index], 0, sizeof(mc_sfx_clips[index]));
    mc_unlock();
}

static void mc_report_audio_error(void)
{
    int error = 0;

    if (!mc_core_ready || mc_audio_error_reported)
        return;

    mc_lock();
    error = mc_audio_error;
    if (error)
        mc_audio_error_reported = 1;
    mc_unlock();

    if (error)
    {
        fprintf(stderr, "MicroWave: audio renderer MUS failure: %s\n",
                snd_mus_error_string(error));
        fflush(stderr);
    }
}

/* ------------------------------------------------------------------------- */
/* Transport-driven mixer                                                      */
/* ------------------------------------------------------------------------- */

static void MC_FD_AUDIO_HOT(mc_render_chunk)(int frames)
{
    int i;
    int emitted;
    int opl_peak = 0;
    int filtered_peak = 0;

    snd_begin_block(&mc_opl_mix, mc_music_frame, frames);

    if (mc_music_active && !mc_music_backend_paused)
    {
        emitted = snd_mus_process_until(&mc_player,
                                        &mc_midi,
                                        mc_music_frame + (long)frames);
        if (emitted > 0)
            mc_music_messages_emitted += (unsigned long)emitted;

        if (emitted < 0)
        {
            mc_audio_error = snd_mus_player_error(&mc_player);
            mc_music_active = 0;
            snd_genmidi_opl_all_sound_off(&mc_opl);
        }

        if (mc_opl.dropped_events != 0uL && mc_audio_error == 0)
            mc_audio_error = SND_MUS_ERR_TIME;

        snd_genmidi_opl_mix_block(&mc_opl, &mc_opl_mix);
        snd_flush_block(&mc_opl_mix);
    }
    else
    {
        snd_clear_block(&mc_opl_mix);
    }

    snd_begin_block(&mc_mix, mc_audio_frame, frames);

    if (mc_music_active && !mc_music_backend_paused)
    {
        snd_touch_block(&mc_mix);

#if SND_WIDE_ACCUM
        if (mc_mix.accum != NULL)
        {
            int32_t *dst = mc_mix.accum;

            for (i = 0; i < frames; ++i)
            {
                long raw = SND_SAMPLE_TO_MIX(mc_opl_block[i]);
                long raw_mag = raw < 0 ? -raw : raw;
                long boosted = raw * (1L << MC_FD_OPL_POST_GAIN_SHIFT);
                long filtered = mc_filter_opl(boosted);
                long filtered_mag = filtered < 0 ? -filtered : filtered;
                unsigned int j = (unsigned int)i * 2u;

                if (raw_mag > opl_peak)
                    opl_peak = (int)raw_mag;
                if (filtered_mag > filtered_peak)
                    filtered_peak = (int)filtered_mag;

                dst[j] += (int32_t)filtered;
                dst[j + 1u] += (int32_t)filtered;
            }
        }
        else
#endif
        {
            for (i = 0; i < frames; ++i)
            {
                long raw = SND_SAMPLE_TO_MIX(mc_opl_block[i]);
                long raw_mag = raw < 0 ? -raw : raw;
                long boosted = raw * (1L << MC_FD_OPL_POST_GAIN_SHIFT);
                long filtered = mc_filter_opl(boosted);
                long filtered_mag = filtered < 0 ? -filtered : filtered;

                if (raw_mag > opl_peak)
                    opl_peak = (int)raw_mag;
                if (filtered_mag > filtered_peak)
                    filtered_peak = (int)filtered_mag;

                snd_block_add(&mc_mix, (long)i * 2L, filtered);
                snd_block_add(&mc_mix, (long)i * 2L + 1L, filtered);
            }
        }

        mc_music_frame += (long)frames;
    }

    mc_music_opl_peak = opl_peak;
    mc_music_filtered_peak = filtered_peak;
    if (opl_peak > mc_music_opl_peak_max)
        mc_music_opl_peak_max = opl_peak;
    if (filtered_peak > mc_music_filtered_peak_max)
        mc_music_filtered_peak_max = filtered_peak;

    snd_bank_mix(&mc_mix, &mc_sfx_bank, NULL);
    snd_flush_block(&mc_mix);

    mc_audio_frame += (long)frames;
}

int mc_fd_audio_core_ready(void)
{
    return mc_core_ready;
}

void mc_fd_audio_set_master_volume(int32_t volume_16_16)
{
    if (volume_16_16 < SND_VOL_SILENT)
        volume_16_16 = SND_VOL_SILENT;
    if (volume_16_16 > SND_VOL_UNITY)
        volume_16_16 = SND_VOL_UNITY;

    mc_master_volume = volume_16_16;

    if (!mc_core_ready)
        return;

    mc_lock();
    snd_set_master_volume(&mc_mix, mc_master_volume);
    mc_unlock();
}

int32_t mc_fd_audio_master_volume(void)
{
    return mc_master_volume;
}

static int mc_music_ascii_equal_ci(const char *a, const char *b)
{
    unsigned char ca;
    unsigned char cb;

    if (!a || !b)
        return 0;

    while (*a && *b)
    {
        ca = (unsigned char)*a++;
        cb = (unsigned char)*b++;

        if (ca >= 'a' && ca <= 'z')
            ca = (unsigned char)(ca - ('a' - 'A'));
        if (cb >= 'a' && cb <= 'z')
            cb = (unsigned char)(cb - ('a' - 'A'));

        if (ca != cb)
            return 0;
    }

    return *a == '\0' && *b == '\0';
}

void mc_fd_audio_get_music_diag(mc_fd_audio_music_diag_t *diag)
{
    if (!diag)
        return;

    memset(diag, 0, sizeof(*diag));
    diag->music_handle = -1;
    snprintf(diag->name, sizeof(diag->name), "%s", "<none>");

    if (!mc_core_ready)
        return;

    mc_lock();

    diag->core_ready = mc_core_ready;
    diag->genmidi_ready = mc_genmidi_ready;
    diag->backend_active = mc_music_active;
    diag->backend_paused = mc_music_backend_paused;
    diag->doom_paused = mc_music_paused;
    diag->music_volume = snd_MusicVolume;

    diag->audio_frame = mc_audio_frame;
    diag->music_frame = mc_music_frame;
    diag->player_start_frame = mc_player.start_frame;
    diag->player_next_frame = mc_player.next_frame;

    diag->messages_emitted = mc_music_messages_emitted;
    diag->loops_completed = mc_player.loops_completed;
    diag->midi_notes_started = mc_opl.midi_notes_started;
    diag->opl_voices_started = mc_opl.opl_voices_started;
    diag->opl_voices_released = mc_opl.opl_voices_released;
    diag->voices_stolen = mc_opl.voices_stolen;
    diag->secondary_voices_dropped = mc_opl.secondary_voices_dropped;
    diag->dropped_events = mc_opl.dropped_events;
    diag->register_writes = mc_opl.register_writes;

    diag->player_cursor = mc_player.cursor;
    diag->player_score_end = mc_player.score_end;
    diag->ticks_in_loop = mc_player.ticks_in_loop;
    diag->player_finished = mc_player.finished;
    diag->player_error = mc_player.error;

    diag->active_voices = snd_genmidi_opl_active_voices(&mc_opl);
    diag->pending_events = snd_genmidi_opl_pending_events(&mc_opl);
    diag->output_gain = mc_opl.output_gain;
    diag->opl_peak = mc_music_opl_peak;
    diag->opl_peak_max = mc_music_opl_peak_max;
    diag->filtered_peak = mc_music_filtered_peak;
    diag->filtered_peak_max = mc_music_filtered_peak_max;
    diag->opl_post_gain_shift = MC_FD_OPL_POST_GAIN_SHIFT;
    diag->opl_post_gain_clips = mc_music_post_gain_clips;

    if (mc_music_playing)
    {
        diag->music_handle = mc_music_playing->handle;
        if (mc_music_playing->name)
            snprintf(diag->name, sizeof(diag->name), "%s",
                     mc_music_playing->name);
    }

    mc_unlock();
}

int mc_fd_audio_debug_change_music(const char *name)
{
    if (mc_music_ascii_equal_ci(name, "INTROA"))
    {
        S_ChangeMusic(mus_introa, 0);
        return 1;
    }

    if (mc_music_ascii_equal_ci(name, "E1M1"))
    {
        S_ChangeMusic(mus_e1m1, 1);
        return 1;
    }

    if (mc_music_ascii_equal_ci(name, "E1M5"))
    {
        S_ChangeMusic(mus_e1m5, 1);
        return 1;
    }

    return 0;
}

void mc_fd_audio_transport_failed(void)
{
    snd_SfxDevice = snd_none;
    snd_MusicDevice = snd_none;
}

void mc_fd_audio_render(unsigned int frames,
                        mc_fd_audio_sink_fn sink,
                        void *user)
{
    unsigned int done = 0;

    if (!sink)
        return;

    if (!mc_core_ready)
    {
        unsigned long i;
        unsigned long samples =
            (unsigned long)MC_FD_AUDIO_BLOCK *
            (unsigned long)MC_FD_AUDIO_CHANNELS;

        for (i = 0; i < samples; ++i)
            mc_mix_block[i] = SND_SAMPLE_SILENCE;

        while (done < frames)
        {
            unsigned int left = frames - done;
            unsigned int count =
                left > (unsigned int)MC_FD_AUDIO_BLOCK
                    ? (unsigned int)MC_FD_AUDIO_BLOCK
                    : left;
            sink(mc_mix_block, count, user);
            done += count;
        }
        return;
    }

    mc_lock();

    while (done < frames)
    {
        unsigned int left = frames - done;
        int count = (left > (unsigned int)MC_FD_AUDIO_BLOCK)
                        ? MC_FD_AUDIO_BLOCK
                        : (int)left;

        mc_render_chunk(count);
        sink(mc_mix.block, (unsigned int)count, user);
        done += (unsigned int)count;
    }

    mc_unlock();
}

static void mc_sound_shutdown(void)
{
    mc_fd_audio_transport_stop();

    if (mc_core_ready)
    {
        mc_lock();
        mc_stop_music_backend_locked();
        snd_bank_stop_all(&mc_sfx_bank);
        mc_unlock();
        mc_core_ready = 0;
        mc_genmidi_ready = 0;
    }

    mc_lock_destroy();
}

/* ------------------------------------------------------------------------- */
/* FastDoom S_* interface                                                     */
/* ------------------------------------------------------------------------- */

int I_GetSfxLumpNum(sfxinfo_t *sfx)
{
    char namebuf[9] = "DS";

    if (!sfx || !sfx->name)
        return 0;

    strcpy(namebuf + 2, sfx->name);
    return W_GetNumForName(namebuf);
}

void S_SetMusicVolume(int volume)
{
    snd_MusicVolume = mc_clampi(volume, 0, MC_FD_MUSIC_MAX);
    mc_set_music_volume_backend(snd_MusicVolume);
}

void S_SetSfxVolume(int volume)
{
    snd_SfxVolume = mc_clampi(volume, 0, MC_FD_SFX_MAX);
}

static void S_StopChannel(int cnum)
{
    mc_fd_channel_t *channel;

    if (!mc_channels || cnum < 0 || cnum >= numChannels)
        return;

    channel = &mc_channels[cnum];
    if (!channel->sfxinfo)
        return;

    if (channel->handle > 0)
        mc_stop_sfx_backend(channel->handle);

    channel->sfxinfo = NULL;
    channel->origin = NULL;
    channel->handle = -1;
}

static int S_GetChannel(void *origin, sfxinfo_t *sfxinfo)
{
    int cnum;
    mc_fd_channel_t *channel;

    for (cnum = 0; cnum < numChannels; ++cnum)
    {
        if (!mc_channels[cnum].sfxinfo)
            break;
        if (origin && mc_channels[cnum].origin == origin)
        {
            S_StopChannel(cnum);
            break;
        }
    }

    if (cnum == numChannels)
    {
        for (cnum = 0; cnum < numChannels; ++cnum)
        {
            if (mc_channels[cnum].sfxinfo->priority >= sfxinfo->priority)
                break;
        }

        if (cnum == numChannels)
            return -1;

        S_StopChannel(cnum);
    }

    channel = &mc_channels[cnum];
    channel->sfxinfo = sfxinfo;
    channel->origin = origin;
    channel->handle = -1;
    return cnum;
}

static byte S_AdjustSoundParams(mobj_t *source, int *vol, int *sep)
{
    fixed_t approx_dist;
    fixed_t adx;
    fixed_t ady;
    angle_t angle;
    fixed_t optSine;

    adx = abs(players_mo->x - source->x);
    ady = abs(players_mo->y - source->y);
    approx_dist = adx + ady - ((adx < ady ? adx : ady) >> 1);

    if (approx_dist > snd_clipping)
        return 1;

    if (approx_dist < S_CLOSE_DIST)
    {
        *vol = snd_SfxVolume;
    }
    else
    {
        *vol = Div1000(snd_SfxVolume *
                       ((snd_clipping - approx_dist) >> FRACBITS));
    }

    if (*vol == 0)
        return 1;

    if (monoSound)
    {
        *sep = NORM_SEP;
    }
    else
    {
        angle = R_PointToAngle2(players_mo->x,
                                players_mo->y,
                                source->x,
                                source->y);
        angle -= players_mo->angle;
        if (angle <= players_mo->angle)
            angle += 0xffffffffu;
        angle >>= ANGLETOFINESHIFT;

        optSine = finesine[angle];
        *sep = 128 - (((optSine << 6) + (optSine << 5)) >> FRACBITS);
    }

    return 0;
}

void S_Init(int sfxVolume, int musicVolume)
{
    int i;
    char namebuf[9];
    int no_sound = M_CheckParm("-nosound") != 0;
    int no_sfx = M_CheckParm("-nosfx") != 0;
    int no_music = M_CheckParm("-nomusic") != 0;

    snd_SfxDevice = (no_sound || no_sfx) ? snd_none : snd_SB;
    snd_MusicDevice = (no_sound || no_music) ? snd_none : snd_Adlib;
    snd_MidiDevice = midi_default;

    /* Save the portable backend as the desired configuration too. Old DOS
     * hardware numbers from FDOOM.CFG should not disable MicroWave. */
    snd_DesiredSfxDevice = snd_SfxDevice;
    snd_DesiredMusicDevice = snd_MusicDevice;
    snd_DesiredMidiDevice = snd_MidiDevice;

    if (numChannels < 1)
        numChannels = 1;
    if (numChannels > SND_BANK_MAX_VOICES)
    {
        fprintf(stderr,
                "MicroWave: clamping FastDoom snd_channels from %d to %d\n",
                numChannels,
                SND_BANK_MAX_VOICES);
        numChannels = SND_BANK_MAX_VOICES;
    }

    mc_channels = (mc_fd_channel_t *)Z_MallocUnowned(
        numChannels * (int)sizeof(*mc_channels), PU_STATIC);
    memset(mc_channels, 0, (size_t)numChannels * sizeof(*mc_channels));
    for (i = 0; i < numChannels; ++i)
        mc_channels[i].handle = -1;

    if (snd_SfxDevice != snd_none)
    {
        for (i = 1; i < NUMSFX; ++i)
            S_sfx[i].lumpnum = I_GetSfxLumpNum(&S_sfx[i]);
    }

    if (snd_MusicDevice != snd_none)
    {
        for (i = 1; i < NUMMUSIC; ++i)
        {
            snprintf(namebuf, sizeof(namebuf), "D_%s", S_music[i].name);
            S_music[i].lumpnum = W_GetNumForName(namebuf);
        }
    }

    if (snd_SfxDevice != snd_none || snd_MusicDevice != snd_none)
    {
        mc_core_init(snd_MusicDevice != snd_none);
        if (snd_MusicDevice != snd_none && !mc_genmidi_ready)
        {
            fprintf(stderr,
                    "MicroWave: disabling music because GENMIDI is unavailable\n");
            snd_MusicDevice = snd_none;
            snd_DesiredMusicDevice = snd_none;
        }
    }

    S_SetSfxVolume(sfxVolume);
    S_SetMusicVolume(musicVolume);
    mc_music_paused = 0;
    mc_music_playing = NULL;
}

void S_StartSound(mobj_t *origin, byte sfx_id)
{
    int sep = NORM_SEP;
    int volume = snd_SfxVolume;
    int cnum;
    int handle;
    sfxinfo_t *sfx;

    (void)mc_fd_audio_transport_start();

    if (snd_SfxDevice == snd_none)
        return;
    if (sfx_id == 0 || sfx_id >= NUMSFX)
        return;

    if (origin && origin != players_mo)
    {
        if (S_AdjustSoundParams(origin, &volume, &sep))
            return;

        if (origin->x == players_mo->x && origin->y == players_mo->y)
            sep = NORM_SEP;
    }

    S_StopSound(origin);

    sfx = &S_sfx[sfx_id];
    cnum = S_GetChannel(origin, sfx);
    if (cnum < 0)
        return;

    if (!sfx->data)
        sfx->data = W_CacheLumpNum(sfx->lumpnum, PU_SOUND);

    handle = mc_start_sfx_backend((int)sfx_id, sfx->data, sep, volume);
    if (handle < 0)
    {
        mc_channels[cnum].sfxinfo = NULL;
        mc_channels[cnum].origin = NULL;
        mc_channels[cnum].handle = -1;
        return;
    }

    mc_channels[cnum].handle = handle;
    mc_fd_audio_transport_service();
}

void S_StopSound(void *origin)
{
    int cnum;

    if (snd_SfxDevice == snd_none || !mc_channels)
        return;

    for (cnum = 0; cnum < numChannels; ++cnum)
    {
        if (mc_channels[cnum].sfxinfo &&
            mc_channels[cnum].origin == origin)
        {
            S_StopChannel(cnum);
            break;
        }
    }
}

void S_UpdateSounds(void)
{
    int cnum;
    int volume;
    int sep;

    (void)mc_fd_audio_transport_start();
    mc_fd_audio_transport_service();
    mc_report_audio_error();

    if (snd_SfxDevice == snd_none || !mc_channels)
        return;

    for (cnum = 0; cnum < numChannels; ++cnum)
    {
        mc_fd_channel_t *channel = &mc_channels[cnum];

        if (!channel->sfxinfo)
            continue;

        if (!mc_sfx_playing_backend(channel->handle))
        {
            S_StopChannel(cnum);
            continue;
        }

        volume = snd_SfxVolume;
        sep = NORM_SEP;

        if (channel->origin && players_mo != channel->origin)
        {
            if (S_AdjustSoundParams((mobj_t *)channel->origin,
                                    &volume,
                                    &sep))
            {
                S_StopChannel(cnum);
                continue;
            }
        }

        mc_update_sfx_backend(channel->handle, sep, volume);
    }
}

void S_ChangeMusic(int musicnum, int looping)
{
    musicinfo_t *music;
    musicinfo_t *old_music;
    int bytes;

    if (snd_MusicDevice == snd_none)
        return;

    if ((snd_MusicDevice == snd_Adlib ||
         snd_MusicDevice == snd_SB) &&
        musicnum == mus_intro)
        musicnum = mus_introa;

    if (musicnum <= mus_None || musicnum >= NUMMUSIC)
        return;

    music = &S_music[musicnum];
    if (mc_music_playing == music)
        return;

    /*
     * Core 0 owns WAD/zone memory.  Load the replacement while the old lump is
     * still valid, then ask Core 1 to atomically stop/reset/start the synth.
     * Only after Core 1 has completed the transition is the old lump freed.
     */
    music->data = W_CacheLumpNum(music->lumpnum, PU_MUSIC);
    bytes = W_LumpLength(music->lumpnum);

    if (!music->data)
        return;

    old_music = mc_music_playing;

    if (!mc_start_music_backend(music->data, bytes, looping))
    {
        Z_Free(music->data);
        music->data = NULL;
        return;
    }

    if (old_music && old_music != music && old_music->data)
    {
        Z_Free(old_music->data);
        old_music->data = NULL;
    }

    music->handle = musicnum;
    mc_music_playing = music;
    mc_music_paused = 0;

    fprintf(stderr,
            "MicroWave music: D_%s (%s, core1-owned)\n",
            music->name,
            looping ? "loop" : "once");
    fflush(stderr);
}

void S_StartMusic(int music_id)
{
    S_ChangeMusic(music_id, 0);
}

void S_StopMusic(void)
{
    musicinfo_t *music = mc_music_playing;

    if (!music)
        return;

    if (!mc_stop_music_backend())
    {
        /*
         * Core 1 did not reach a safe chunk boundary.  Keep both the backend
         * and the WAD lump alive rather than freeing memory it may still be
         * reading.
         */
        return;
    }

    if (music->data)
    {
        Z_Free(music->data);
        music->data = NULL;
    }

    mc_music_playing = NULL;
    mc_music_paused = 0;
}

void S_PauseMusic(void)
{
    if (mc_music_playing && !mc_music_paused)
    {
        mc_pause_music_backend();
        mc_music_paused = 1;
    }
}

void S_ResumeMusic(void)
{
    if (mc_music_playing && mc_music_paused)
    {
        mc_resume_music_backend();
        mc_music_paused = 0;
    }
}

void S_Start(void)
{
    int cnum;
    int musicnum;

    if (mc_channels)
    {
        for (cnum = 0; cnum < numChannels; ++cnum)
        {
            if (mc_channels[cnum].sfxinfo)
                S_StopChannel(cnum);
        }
    }

    mc_music_paused = 0;

    if (gamemode == commercial)
    {
        musicnum = mus_runnin + gamemap - 1;
    }
    else
    {
        static const byte episode4_music[9] = {
            mus_e3m4,
            mus_e3m2,
            mus_e3m3,
            mus_e1m5,
            mus_e2m7,
            mus_e2m4,
            mus_e2m6,
            mus_e2m5,
            mus_e1m9
        };

        if (gameepisode < 4)
            musicnum = mus_e1m1 + (gameepisode - 1) * 9 + gamemap - 1;
        else
            musicnum = episode4_music[gamemap - 1];
    }

    S_ChangeMusic(musicnum, 1);
}

void S_ClearSounds(void)
{
    int i;

    if (snd_SfxDevice == snd_none)
        return;

    for (i = 1; i < NUMSFX; ++i)
    {
        if (S_sfx[i].data)
        {
            mc_invalidate_sfx_clip(i);
            Z_ChangeTag(S_sfx[i].data, PU_CACHE);
            S_sfx[i].data = NULL;
        }
    }
}

void S_ClearUnusedSounds(void)
{
    int i;
    int j;

    if (snd_SfxDevice == snd_none || !mc_channels)
        return;

    for (i = 1; i < NUMSFX; ++i)
    {
        int unused = 1;

        if (!S_sfx[i].data)
            continue;

        for (j = 0; j < numChannels; ++j)
        {
            if (mc_channels[j].sfxinfo == &S_sfx[i])
            {
                unused = 0;
                break;
            }
        }

        if (unused)
        {
            mc_invalidate_sfx_clip(i);
            Z_ChangeTag(S_sfx[i].data, PU_CACHE);
            S_sfx[i].data = NULL;
        }
    }
}

void S_CheckCD(void) {}
void S_CheckWAV(void) {}
void S_ShowMusicTitle(int musicnum) { (void)musicnum; }
