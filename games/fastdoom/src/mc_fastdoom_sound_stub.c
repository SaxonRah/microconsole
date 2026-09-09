/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Silent FastDoom sound boundary for the first Raylib bring-up.
 *
 * Keep the full game-facing sound API intact so MicroWave can replace this
 * file later without touching gameplay.
 */
#include "doomtype.h"
#include "i_sound.h"
#include "s_sound.h"
#include "sounds.h"

int snd_Mport = 0;
int snd_Sport = 0;
int snd_Rate = 2;
int snd_PCMRate = 1;
int snd_GoldIrq = 0;
int snd_GoldDma = 0;

int snd_MusicVolume = 0;
int snd_SfxVolume = 0;

int snd_SfxDevice = snd_none;
int snd_MusicDevice = snd_none;
int snd_MidiDevice = midi_default;

int snd_DesiredSfxDevice = snd_none;
int snd_DesiredMusicDevice = snd_none;
int snd_DesiredMidiDevice = midi_default;

/* s_sound.c normally owns this; the silent bring-up stub must still provide
 * the gameplay-visible clipping distance used by G_DoLoadLevel(). */
int snd_clipping = S_CLIPPING_DIST;

int I_GetSfxLumpNum(sfxinfo_t *sfx)
{
    (void)sfx;
    return 0;
}

void S_Init(int sfx_volume, int music_volume)
{
    snd_SfxVolume = sfx_volume;
    snd_MusicVolume = music_volume;

    snd_SfxDevice = snd_none;
    snd_MusicDevice = snd_none;
    snd_MidiDevice = midi_default;
}

void S_Start(void) {}
void S_StartSound(mobj_t *origin, byte sound_id)
{
    (void)origin;
    (void)sound_id;
}
void S_StopSound(void *origin) { (void)origin; }
void S_StartMusic(int music_id) { (void)music_id; }
void S_ChangeMusic(int music_id, int looping)
{
    (void)music_id;
    (void)looping;
}
void S_StopMusic(void) {}
void S_PauseMusic(void) {}
void S_ResumeMusic(void) {}
void S_CheckCD(void) {}
void S_CheckWAV(void) {}
void S_UpdateSounds(void) {}

void S_SetMusicVolume(int volume)
{
    snd_MusicVolume = volume;
}

void S_SetSfxVolume(int volume)
{
    snd_SfxVolume = volume;
}

void S_ClearSounds(void) {}
void S_ClearUnusedSounds(void) {}
void S_ShowMusicTitle(int musicnum) { (void)musicnum; }
