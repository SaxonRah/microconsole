#include "mc_example.h"

#include <string.h>

/* Grouped by generation, oldest first within each group, so stepping forward
   through the list walks forward through the two eras.
 *
 * The 8-bit group opens with the 2600 because everything after it is a
 * reaction to what the 2600 did not have. The 16-bit group opens with the PC
 * Engine, which shipped an 8-bit CPU behind a video chip nobody would call
 * 8-bit, and closes with the Neo Geo.
 *
 * A table of accessors rather than a table of structs: each example owns its
 * own descriptor next to the code it describes, and the registry never needs
 * to know how large any example's state is.
 */
typedef const mc_example_t *(*mc_example_fn)(void);

static const mc_example_fn mc_table[] = {
    mc_example_a2600_beam,     /* 1977 Atari VCS / 2600       */
    mc_example_nes_split,      /* 1983 Famicom / NES          */
    mc_example_sms_lock,       /* 1985 Sega Master System     */
    mc_example_gb_window,      /* 1989 Game Boy               */
    mc_example_pce_wavetable,  /* 1987 PC Engine / TG-16      */
    mc_example_md_linescroll,  /* 1988 Mega Drive / Genesis   */
    mc_example_snes_mode7,     /* 1990 Super Famicom / SNES   */
    mc_example_snes_colormath, /* 1990 Super Famicom / SNES   */
    mc_example_neogeo_zoom     /* 1990 Neo Geo MVS/AES        */
};

int mc_example_count(void) {
  return (int)(sizeof(mc_table) / sizeof(mc_table[0]));
}

const mc_example_t *mc_example_at(int index) {
  if (index < 0 || index >= mc_example_count())
    return 0;
  return mc_table[index]();
}

int mc_example_find(const char *id) {
  int i;
  if (!id)
    return -1;
  for (i = 0; i < mc_example_count(); ++i) {
    const mc_example_t *e = mc_example_at(i);
    if (e && e->id && strcmp(e->id, id) == 0)
      return i;
  }
  return -1;
}
