/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Persistent SD-card WAD selector for the FastDoom Pico target.
 *
 * MCWAD.CFG format (deliberately tiny and human-readable):
 *
 *   MCWAD2
 *   <state> <mode> <selected>
 *   <base>
 *
 * state:
 *   C = confirmed
 *   P = pending first boot
 *   T = trial is currently being attempted
 *
 * mode:
 *   I = selected is an IWAD and is also the base
 *   P = selected is a PWAD loaded with "-iwad <base> -file <selected>"
 *
 * If a T state survives a reset, FastDoom did not reach I_StartTic() and the
 * selector automatically returns to doom.wad. This matters because an IWAD
 * header alone does not mean that the file belongs to the Doom game family;
 * Heretic/Hexen/Strife IWADs use the same container signature.
 */

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ff.h"

#include "mc_fastdoom_pico_wad.h"

/* Implemented by mc_fastdoom_pico_fs.c. */
int mc_fd_pico_fs_ready(void);
int mc_fd_pico_fs_probe_wad(void);

#define MC_FD_WAD_CFG_PATH "MCWAD.CFG"
#define MC_FD_WAD_DEFAULT  "doom.wad"

typedef struct mc_fd_wad_config_s
{
    char state;
    char mode;
    char selected[MC_FD_WAD_NAME_MAX];
    char base[MC_FD_WAD_NAME_MAX];
} mc_fd_wad_config_t;

static mc_fd_wad_config_t g_mc_fd_wad_cfg = {
    'C', 'I', MC_FD_WAD_DEFAULT, MC_FD_WAD_DEFAULT
};
static int g_mc_fd_wad_cfg_loaded;

static int mc_fd_wad_equal_ci(char a, char b)
{
    unsigned char ua = (unsigned char)a;
    unsigned char ub = (unsigned char)b;

    if (ua >= 'a' && ua <= 'z')
        ua = (unsigned char)(ua - ('a' - 'A'));
    if (ub >= 'a' && ub <= 'z')
        ub = (unsigned char)(ub - ('a' - 'A'));

    return ua == ub;
}

static int mc_fd_wad_ends_with_ci(const char *text, const char *suffix)
{
    size_t nt;
    size_t ns;
    size_t i;

    if (!text || !suffix)
        return 0;

    nt = strlen(text);
    ns = strlen(suffix);
    if (nt < ns)
        return 0;

    text += nt - ns;
    for (i = 0u; i < ns; ++i)
    {
        if (!mc_fd_wad_equal_ci(text[i], suffix[i]))
            return 0;
    }

    return 1;
}

static void mc_fd_wad_default_config(mc_fd_wad_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->state = 'C';
    cfg->mode = 'I';
    strcpy(cfg->selected, MC_FD_WAD_DEFAULT);
    strcpy(cfg->base, MC_FD_WAD_DEFAULT);
}

static int mc_fd_wad_ensure_fs(void)
{
    if (!mc_fd_pico_fs_ready())
        (void)mc_fd_pico_fs_probe_wad();

    return mc_fd_pico_fs_ready();
}

static int mc_fd_wad_make_filename(const char *request,
                                   char out[MC_FD_WAD_NAME_MAX])
{
    const char *begin;
    const char *end;
    size_t n;
    size_t i;

    if (!request)
        return 0;

    begin = request;
    while (*begin == ' ' || *begin == '\t')
        ++begin;

    end = begin + strlen(begin);
    while (end > begin &&
           (end[-1] == ' ' || end[-1] == '\t' ||
            end[-1] == '\r' || end[-1] == '\n'))
        --end;

    n = (size_t)(end - begin);
    if (n == 0u || n >= MC_FD_WAD_NAME_MAX - 4u)
        return 0;

    for (i = 0u; i < n; ++i)
    {
        unsigned char c = (unsigned char)begin[i];

        /*
         * The shell selects files from the SD root only. Spaces are rejected
         * because MCWAD.CFG intentionally uses whitespace-separated tokens.
         */
        if (c <= ' ' || c == '/' || c == '\\' || c == ':')
            return 0;

        out[i] = (char)c;
    }
    out[n] = '\0';

    if (!mc_fd_wad_ends_with_ci(out, ".wad"))
    {
        strcat(out, ".wad");
    }

    return 1;
}

static int mc_fd_wad_probe(const char *path, char *mode_out, FSIZE_t *size_out)
{
    FIL file;
    BYTE header[4];
    UINT got = 0u;
    FRESULT fr;

    if (!mc_fd_wad_ensure_fs())
        return 0;

    fr = f_open(&file, path, FA_READ | FA_OPEN_EXISTING);
    if (fr != FR_OK)
        return 0;

    if (size_out)
        *size_out = f_size(&file);

    fr = f_read(&file, header, sizeof(header), &got);
    (void)f_close(&file);

    if (fr != FR_OK || got != sizeof(header))
        return 0;

    if (memcmp(header, "IWAD", 4u) == 0)
    {
        if (mode_out)
            *mode_out = 'I';
        return 1;
    }

    if (memcmp(header, "PWAD", 4u) == 0)
    {
        if (mode_out)
            *mode_out = 'P';
        return 1;
    }

    return 0;
}

static int mc_fd_wad_write_config(const mc_fd_wad_config_t *cfg)
{
    FIL file;
    char text[256];
    int n;
    UINT put = 0u;
    FRESULT fr;

    if (!cfg || !mc_fd_wad_ensure_fs())
        return 0;

    n = snprintf(text, sizeof(text),
                 "MCWAD2\n%c %c %s\n%s\n",
                 cfg->state,
                 cfg->mode,
                 cfg->selected,
                 cfg->base);
    if (n <= 0 || (size_t)n >= sizeof(text))
        return 0;

    fr = f_open(&file, MC_FD_WAD_CFG_PATH,
                FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK)
        return 0;

    fr = f_write(&file, text, (UINT)n, &put);
    if (fr == FR_OK && put == (UINT)n)
        fr = f_sync(&file);

    (void)f_close(&file);
    return fr == FR_OK && put == (UINT)n;
}

static int mc_fd_wad_read_config(mc_fd_wad_config_t *cfg)
{
    FIL file;
    char text[256];
    UINT got = 0u;
    FRESULT fr;
    char state;
    char mode;
    char selected[MC_FD_WAD_NAME_MAX];
    char base[MC_FD_WAD_NAME_MAX];
    int parsed;

    if (!cfg || !mc_fd_wad_ensure_fs())
        return 0;

    fr = f_open(&file, MC_FD_WAD_CFG_PATH,
                FA_READ | FA_OPEN_EXISTING);
    if (fr != FR_OK)
        return 0;

    fr = f_read(&file, text, (UINT)(sizeof(text) - 1u), &got);
    (void)f_close(&file);

    if (fr != FR_OK)
        return 0;

    text[got] = '\0';

    parsed = sscanf(text,
                    "MCWAD2\n%c %c %63s\n%63s",
                    &state, &mode, selected, base);
    if (parsed != 4)
        return 0;

    if ((state != 'C' && state != 'P' && state != 'T') ||
        (mode != 'I' && mode != 'P'))
        return 0;

    memset(cfg, 0, sizeof(*cfg));
    cfg->state = state;
    cfg->mode = mode;
    strcpy(cfg->selected, selected);
    strcpy(cfg->base, base);
    return 1;
}

static void mc_fd_wad_export(const mc_fd_wad_config_t *cfg,
                             mc_fd_wad_boot_t *out)
{
    if (!out || !cfg)
        return;

    memset(out, 0, sizeof(*out));
    strcpy(out->selected, cfg->selected);
    strcpy(out->base, cfg->base);
    out->mode = (cfg->mode == 'P')
                    ? MC_FD_WAD_MODE_PWAD
                    : MC_FD_WAD_MODE_IWAD;
    out->state = cfg->state;
}

static int mc_fd_wad_config_files_exist(const mc_fd_wad_config_t *cfg)
{
    char mode;

    if (!cfg)
        return 0;

    if (!mc_fd_wad_probe(cfg->selected, &mode, NULL))
        return 0;

    if (cfg->mode == 'I')
        return mode == 'I';

    if (mode != 'P')
        return 0;

    if (!mc_fd_wad_probe(cfg->base, &mode, NULL))
        return 0;

    return mode == 'I';
}

int mc_fd_pico_wad_boot_load(mc_fd_wad_boot_t *out)
{
    mc_fd_wad_config_t cfg;

    mc_fd_wad_default_config(&cfg);

    if (mc_fd_wad_ensure_fs())
    {
        mc_fd_wad_config_t disk;

        if (mc_fd_wad_read_config(&disk))
            cfg = disk;

        /*
         * T surviving a reset means the selected WAD never reached the game
         * loop. Recover to the known Doom baseline.
         */
        if (cfg.state == 'T')
        {
            printf("MCFDOOM1 wad_fallback=%s failed=%s\n",
                   MC_FD_WAD_DEFAULT, cfg.selected);
            mc_fd_wad_default_config(&cfg);
            (void)mc_fd_wad_write_config(&cfg);
        }
        else if (!mc_fd_wad_config_files_exist(&cfg))
        {
            printf("MCFDOOM1 wad_fallback=%s reason=missing-or-invalid\n",
                   MC_FD_WAD_DEFAULT);
            mc_fd_wad_default_config(&cfg);
            (void)mc_fd_wad_write_config(&cfg);
        }
        else if (cfg.state == 'P')
        {
            cfg.state = 'T';
            if (!mc_fd_wad_write_config(&cfg))
            {
                /*
                 * Do not attempt an untracked risky boot. Stay on the known
                 * baseline if the trial marker cannot be made durable.
                 */
                mc_fd_wad_default_config(&cfg);
            }
            else
            {
                printf("MCFDOOM1 wad_trial=%s mode=%s base=%s\n",
                       cfg.selected,
                       cfg.mode == 'P' ? "PWAD" : "IWAD",
                       cfg.base);
            }
        }
    }

    g_mc_fd_wad_cfg = cfg;
    g_mc_fd_wad_cfg_loaded = 1;

    mc_fd_wad_export(&cfg, out);
    return 1;
}

int mc_fd_pico_wad_confirm_boot(void)
{
    if (!g_mc_fd_wad_cfg_loaded)
        return 0;

    if (g_mc_fd_wad_cfg.state != 'T')
        return 1;

    g_mc_fd_wad_cfg.state = 'C';

    if (!mc_fd_wad_write_config(&g_mc_fd_wad_cfg))
    {
        g_mc_fd_wad_cfg.state = 'T';
        return 0;
    }

    printf("MCFDOOM1 wad_confirm=%s mode=%s base=%s\n",
           g_mc_fd_wad_cfg.selected,
           g_mc_fd_wad_cfg.mode == 'P' ? "PWAD" : "IWAD",
           g_mc_fd_wad_cfg.base);
    fflush(stdout);
    return 1;
}

int mc_fd_pico_wad_current(mc_fd_wad_boot_t *out)
{
    if (!g_mc_fd_wad_cfg_loaded)
        (void)mc_fd_pico_wad_boot_load(NULL);

    mc_fd_wad_export(&g_mc_fd_wad_cfg, out);
    return 1;
}

int mc_fd_pico_wad_select(const char *request, mc_fd_wad_boot_t *out)
{
    mc_fd_wad_config_t cfg;
    char filename[MC_FD_WAD_NAME_MAX];
    char mode;

    if (!mc_fd_wad_make_filename(request, filename))
        return 0;

    if (!mc_fd_wad_probe(filename, &mode, NULL))
        return 0;

    if (!g_mc_fd_wad_cfg_loaded)
        (void)mc_fd_pico_wad_boot_load(NULL);

    memset(&cfg, 0, sizeof(cfg));
    cfg.state = 'P';
    cfg.mode = mode;
    strcpy(cfg.selected, filename);

    if (mode == 'I')
    {
        strcpy(cfg.base, filename);
    }
    else
    {
        /*
         * A PWAD overlays the currently selected base IWAD. If the current
         * selection is itself a PWAD, g_mc_fd_wad_cfg.base still names the
         * underlying IWAD.
         */
        strcpy(cfg.base, g_mc_fd_wad_cfg.base[0]
                             ? g_mc_fd_wad_cfg.base
                             : MC_FD_WAD_DEFAULT);

        if (!mc_fd_wad_probe(cfg.base, &mode, NULL) || mode != 'I')
            strcpy(cfg.base, MC_FD_WAD_DEFAULT);

        cfg.mode = 'P';
    }

    if (!mc_fd_wad_write_config(&cfg))
        return -1;

    g_mc_fd_wad_cfg = cfg;
    g_mc_fd_wad_cfg_loaded = 1;
    mc_fd_wad_export(&cfg, out);
    return 1;
}

int mc_fd_pico_wad_list(void)
{
    DIR dir;
    FILINFO info;
    FRESULT fr;
    int count = 0;

    if (!mc_fd_wad_ensure_fs())
        return -1;

    fr = f_opendir(&dir, "");
    if (fr != FR_OK)
        return -1;

    for (;;)
    {
        char mode = '?';
        FSIZE_t size = 0u;

        fr = f_readdir(&dir, &info);
        if (fr != FR_OK || info.fname[0] == '\0')
            break;

        if (info.fattrib & AM_DIR)
            continue;

        if (!mc_fd_wad_ends_with_ci(info.fname, ".wad"))
            continue;

        if (mc_fd_wad_probe(info.fname, &mode, &size))
        {
            printf("MCFDOOM1 wad_entry=%s type=%s size=%lu\n",
                   info.fname,
                   mode == 'I' ? "IWAD" : "PWAD",
                   (unsigned long)size);
        }
        else
        {
            printf("MCFDOOM1 wad_entry=%s type=UNKNOWN size=%lu\n",
                   info.fname,
                   (unsigned long)info.fsize);
        }

        ++count;
    }

    (void)f_closedir(&dir);

    if (fr != FR_OK)
        return -1;

    return count;
}
