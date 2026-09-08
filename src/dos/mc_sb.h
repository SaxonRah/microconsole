#ifndef MC_SB_H
#define MC_SB_H

#include "snd.h"

int mc_sb_init(int initial_volume);

/* Redirect the transport at a different mix scene.
 *
 * The Sound Blaster DMA plumbing is MicroConsole's, not MicroWave's, and it
 * should not care which program is filling the block -- exactly as the Raylib
 * frontend passes whatever mix_scene it likes to snd_render_one_block(). The
 * default remains the shared music demo, so MCDEMO is unchanged; the examples
 * build points this at the selected example's mix().
 *
 * Passing a NULL mix restores the default. */
void mc_sb_set_scene(void (*mix)(snd_mixer_t SND_PTR *m, void SND_PTR *user),
                     void SND_PTR *user);

/* The mixer the transport owns, so a caller can be told the rate and block
   size it has to schedule against. NULL before mc_sb_init() succeeds. */
const snd_mixer_t SND_PTR *mc_sb_mixer(void);

void mc_sb_set_volume(int volume);
void mc_sb_service(void);
void mc_sb_shutdown(void);
unsigned long mc_sb_frames(void);
#endif
