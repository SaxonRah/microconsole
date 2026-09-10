#ifndef MC_FASTDOOM_PICO_WAD_H
#define MC_FASTDOOM_PICO_WAD_H

#ifdef __cplusplus
extern "C" {
#endif

#define MC_FD_WAD_NAME_MAX 64

enum
{
    MC_FD_WAD_MODE_IWAD = 0,
    MC_FD_WAD_MODE_PWAD = 1
};

typedef struct mc_fd_wad_boot_s
{
    char selected[MC_FD_WAD_NAME_MAX];
    char base[MC_FD_WAD_NAME_MAX];
    int mode;
    char state;
} mc_fd_wad_boot_t;

/*
 * Resolve the persisted SD-card selection for this boot.
 *
 * A freshly selected WAD is a trial. The first boot marks it "trying";
 * mc_fd_pico_wad_confirm_boot() promotes it only after FastDoom reaches
 * I_StartTic(). If the trial never reaches the game loop, the next reset
 * falls back to doom.wad.
 */
int mc_fd_pico_wad_boot_load(mc_fd_wad_boot_t *out);
int mc_fd_pico_wad_confirm_boot(void);

/*
 * Select a root-level .wad by basename ("doom2" or "doom2.wad").
 * IWADs become the new base. PWADs are launched with the current base IWAD
 * through FastDoom's -file path.
 *
 * Returns:
 *   1  selected and persisted as a trial
 *   0  not found / invalid WAD / invalid name
 *  -1  filesystem/config write failure
 */
int mc_fd_pico_wad_select(const char *request, mc_fd_wad_boot_t *out);

/* Current persisted/boot selection. */
int mc_fd_pico_wad_current(mc_fd_wad_boot_t *out);

/*
 * Print all root-level .wad files as MCFDOOM1 wad_entry=... records.
 * Returns number of WADs listed, or -1 on filesystem failure.
 */
int mc_fd_pico_wad_list(void);

#ifdef __cplusplus
}
#endif

#endif
