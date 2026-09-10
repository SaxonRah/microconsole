/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * FastDoom Pico filesystem bridge.
 *
 * Strong definitions of newlib's weak _open/_close/_read/_write/_lseek hooks
 * route ordinary files to FatFs on the microSD card while preserving Pico USB
 * stdio on descriptors 0, 1 and 2.
 *
 * FastDoom's generated portability overlay uses these descriptor functions for
 * WADs, config files, saves, demos, and benchmark output.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "pico/stdio.h"
#include "hardware/gpio.h"

#include "ff.h"
#include "diskio.h"
#include "hw_config.h"
#include "sd_card.h"

#include "dos.h"
#include "io.h"

#ifndef MC_FD_MAX_FILES
#define MC_FD_MAX_FILES 8
#endif

#define MC_FD_STDIO_COUNT 3
#define MC_FD_PATH_MAX 260

typedef struct mc_fd_file_slot_s
{
    int used;
    FIL file;
} mc_fd_file_slot_t;

typedef struct mc_fd_find_slot_s
{
    int used;
    DIR dir;
    FILINFO info;
} mc_fd_find_slot_t;

static mc_fd_file_slot_t g_mc_fd_files[MC_FD_MAX_FILES];
static mc_fd_find_slot_t g_mc_fd_finds[2];

static int g_mc_fd_fs_ready;
static int g_mc_fd_sd_driver_ok = -1;
static FRESULT g_mc_fd_mount_result = FR_NOT_READY;
static FRESULT g_mc_fd_last_result = FR_NOT_READY;
static unsigned int g_mc_fd_mount_attempts;
static char g_mc_fd_last_path[MC_FD_PATH_MAX] = "<none>";
static DSTATUS g_mc_fd_raw_dstatus = STA_NOINIT;
static int g_mc_fd_raw_card_type = -1;
static int g_mc_fd_raw_miso = -1;

static int mc_fd_errno_from_fresult(FRESULT fr)
{
    switch (fr)
    {
    case FR_OK:
        return 0;
    case FR_NO_FILE:
    case FR_NO_PATH:
        return ENOENT;
    case FR_INVALID_NAME:
    case FR_INVALID_PARAMETER:
        return EINVAL;
    case FR_DENIED:
        return EACCES;
    case FR_EXIST:
        return EEXIST;
    case FR_WRITE_PROTECTED:
        return EROFS;
    case FR_NOT_ENOUGH_CORE:
        return ENOMEM;
    case FR_TOO_MANY_OPEN_FILES:
        return EMFILE;
    case FR_NOT_READY:
    case FR_INVALID_DRIVE:
    case FR_NOT_ENABLED:
    case FR_NO_FILESYSTEM:
        return ENODEV;
    default:
        return EIO;
    }
}

static void mc_fd_set_errno(FRESULT fr)
{
    errno = mc_fd_errno_from_fresult(fr);
}

static const char *mc_fd_normalize_path(const char *src,
                                        char dst[MC_FD_PATH_MAX])
{
    size_t n = 0u;

    if (src == NULL)
        return NULL;

    while (*src == '/' || *src == '\\')
        ++src;

    while (*src != '\0' && n + 1u < MC_FD_PATH_MAX)
    {
        char c = *src++;
        dst[n++] = (c == '\\') ? '/' : c;
    }

    if (*src != '\0')
    {
        errno = ENAMETOOLONG;
        return NULL;
    }

    dst[n] = '\0';
    return dst;
}

static void mc_fd_record_path(const char *path, FRESULT fr)
{
    if (path != NULL)
    {
        strncpy(g_mc_fd_last_path, path, sizeof(g_mc_fd_last_path) - 1u);
        g_mc_fd_last_path[sizeof(g_mc_fd_last_path) - 1u] = '\0';
    }
    g_mc_fd_last_result = fr;
}

static int mc_fd_fs_ensure_ready(void)
{
    sd_card_t *sd;
    const char *drive;

    if (g_mc_fd_fs_ready)
        return 1;

    /*
     * Do not latch a failed first attempt forever.
     *
     * FastDoom's first filesystem request is TEXT/PROG.TXT, before it probes
     * doom.wad.  On real cards the first SPI/FatFs initialization can be the
     * slowest transaction after reset, so every later filesystem request gets
     * another chance until the volume has mounted successfully.
     */
    ++g_mc_fd_mount_attempts;

    if (!sd_init_driver())
    {
        g_mc_fd_sd_driver_ok = 0;
        g_mc_fd_mount_result = FR_NOT_READY;
        g_mc_fd_last_result = FR_NOT_READY;
        errno = ENODEV;
        return 0;
    }

    g_mc_fd_sd_driver_ok = 1;

    sd = sd_get_by_num(0u);
    if (sd == NULL)
    {
        g_mc_fd_mount_result = FR_INVALID_DRIVE;
        g_mc_fd_last_result = FR_INVALID_DRIVE;
        errno = ENODEV;
        return 0;
    }

    drive = sd_get_drive_prefix(sd);
    g_mc_fd_mount_result = f_mount(&sd->state.fatfs, drive, 1);

    if (g_mc_fd_mount_result != FR_OK)
    {
        g_mc_fd_last_result = g_mc_fd_mount_result;
        mc_fd_set_errno(g_mc_fd_mount_result);
        return 0;
    }

    sd->state.mounted = true;
    g_mc_fd_fs_ready = 1;
    g_mc_fd_last_result = FR_OK;
    return 1;
}

int mc_fd_pico_fs_ready(void)
{
    return g_mc_fd_fs_ready;
}

int mc_fd_pico_fs_sd_driver_ok(void)
{
    return g_mc_fd_sd_driver_ok;
}

int mc_fd_pico_fs_mount_result(void)
{
    return (int)g_mc_fd_mount_result;
}

int mc_fd_pico_fs_last_result(void)
{
    return (int)g_mc_fd_last_result;
}

unsigned int mc_fd_pico_fs_mount_attempts(void)
{
    return g_mc_fd_mount_attempts;
}

const char *mc_fd_pico_fs_last_path(void)
{
    return g_mc_fd_last_path;
}

int mc_fd_pico_fs_probe_wad(void)
{
    FILINFO info;
    FRESULT fr;

    if (!mc_fd_fs_ensure_ready())
        return 0;

    fr = f_stat("doom.wad", &info);
    mc_fd_record_path("doom.wad", fr);

    if (fr != FR_OK)
    {
        mc_fd_set_errno(fr);
        return 0;
    }

    return 1;
}

static BYTE mc_fd_fatfs_mode(int oflag)
{
    BYTE mode = 0u;
    int access = oflag & O_ACCMODE;

    if (access == O_WRONLY)
        mode |= FA_WRITE;
    else if (access == O_RDWR)
        mode |= FA_READ | FA_WRITE;
    else
        mode |= FA_READ;

    if (oflag & O_CREAT)
    {
        if (oflag & O_EXCL)
            mode |= FA_CREATE_NEW;
        else if (oflag & O_TRUNC)
            mode |= FA_CREATE_ALWAYS;
        else
            mode |= FA_OPEN_ALWAYS;
    }
    else if ((oflag & O_TRUNC) && (mode & FA_WRITE))
    {
        mode |= FA_CREATE_ALWAYS;
    }
    else
    {
        mode |= FA_OPEN_EXISTING;
    }

    return mode;
}

static mc_fd_file_slot_t *mc_fd_slot_from_fd(int fd)
{
    int index = fd - MC_FD_STDIO_COUNT;

    if (index < 0 || index >= MC_FD_MAX_FILES)
        return NULL;
    if (!g_mc_fd_files[index].used)
        return NULL;

    return &g_mc_fd_files[index];
}


int mc_fd_pico_fs_raw_probe(void)
{
    sd_card_t *sd;

    /*
     * disk_initialize() is the exact low-level entry FatFs calls when
     * f_mount(..., 1) tries to bring the medium online.
     */
    g_mc_fd_raw_dstatus = disk_initialize(0);

    sd = sd_get_by_num(0u);
    if (sd != NULL)
    {
        g_mc_fd_raw_card_type = (int)sd->state.card_type;
        g_mc_fd_raw_miso = gpio_get(MC_SD_MISO) ? 1 : 0;
    }
    else
    {
        g_mc_fd_raw_card_type = -1;
        g_mc_fd_raw_miso = gpio_get(MC_SD_MISO) ? 1 : 0;
    }

    return (int)g_mc_fd_raw_dstatus;
}

int mc_fd_pico_fs_raw_dstatus(void)
{
    return (int)g_mc_fd_raw_dstatus;
}

int mc_fd_pico_fs_raw_card_type(void)
{
    return g_mc_fd_raw_card_type;
}

int mc_fd_pico_fs_raw_miso(void)
{
    return g_mc_fd_raw_miso;
}

int _open(const char *filename, int oflag, ...)
{
    char path[MC_FD_PATH_MAX];
    const char *fat_path;
    int i;
    FRESULT fr;
    BYTE mode;

    if (!mc_fd_fs_ensure_ready())
        return -1;

    fat_path = mc_fd_normalize_path(filename, path);
    if (fat_path == NULL)
        return -1;

    for (i = 0; i < MC_FD_MAX_FILES; ++i)
    {
        if (!g_mc_fd_files[i].used)
            break;
    }

    if (i == MC_FD_MAX_FILES)
    {
        errno = EMFILE;
        return -1;
    }

    mode = mc_fd_fatfs_mode(oflag);
    fr = f_open(&g_mc_fd_files[i].file, fat_path, mode);
    mc_fd_record_path(fat_path, fr);
    if (fr != FR_OK)
    {
        mc_fd_set_errno(fr);
        return -1;
    }

    if (oflag & O_APPEND)
    {
        fr = f_lseek(&g_mc_fd_files[i].file, f_size(&g_mc_fd_files[i].file));
        if (fr != FR_OK)
        {
            (void)f_close(&g_mc_fd_files[i].file);
            mc_fd_set_errno(fr);
            return -1;
        }
    }

    g_mc_fd_files[i].used = 1;
    return i + MC_FD_STDIO_COUNT;
}

int _close(int fd)
{
    mc_fd_file_slot_t *slot;
    FRESULT fr;

    if (fd >= 0 && fd < MC_FD_STDIO_COUNT)
        return 0;

    slot = mc_fd_slot_from_fd(fd);
    if (slot == NULL)
    {
        errno = EBADF;
        return -1;
    }

    fr = f_close(&slot->file);
    slot->used = 0;

    if (fr != FR_OK)
    {
        mc_fd_set_errno(fr);
        return -1;
    }

    return 0;
}

int _read(int fd, char *buffer, int count)
{
    mc_fd_file_slot_t *slot;
    UINT got = 0u;
    FRESULT fr;

    if (fd == 0)
        return stdio_get_until(buffer, count, at_the_end_of_time);

    slot = mc_fd_slot_from_fd(fd);
    if (slot == NULL)
    {
        errno = EBADF;
        return -1;
    }

    if (count < 0)
    {
        errno = EINVAL;
        return -1;
    }

    fr = f_read(&slot->file, buffer, (UINT)count, &got);
    if (fr != FR_OK)
    {
        mc_fd_set_errno(fr);
        return -1;
    }

    return (int)got;
}

int _write(int fd, char *buffer, int count)
{
    mc_fd_file_slot_t *slot;
    UINT put = 0u;
    FRESULT fr;

    if (fd == 1 || fd == 2)
    {
        stdio_put_string(buffer, count, false, true);
        return count;
    }

    slot = mc_fd_slot_from_fd(fd);
    if (slot == NULL)
    {
        errno = EBADF;
        return -1;
    }

    if (count < 0)
    {
        errno = EINVAL;
        return -1;
    }

    fr = f_write(&slot->file, buffer, (UINT)count, &put);
    if (fr != FR_OK)
    {
        mc_fd_set_errno(fr);
        return -1;
    }

    return (int)put;
}

off_t _lseek(int fd, off_t offset, int origin)
{
    mc_fd_file_slot_t *slot;
    FSIZE_t base;
    FSIZE_t next;
    FRESULT fr;

    slot = mc_fd_slot_from_fd(fd);
    if (slot == NULL)
    {
        errno = EBADF;
        return (off_t)-1;
    }

    if (origin == SEEK_SET)
    {
        if (offset < 0)
        {
            errno = EINVAL;
            return (off_t)-1;
        }
        base = 0u;
    }
    else if (origin == SEEK_CUR)
    {
        base = f_tell(&slot->file);
    }
    else if (origin == SEEK_END)
    {
        base = f_size(&slot->file);
    }
    else
    {
        errno = EINVAL;
        return (off_t)-1;
    }

    if (offset < 0)
    {
        FSIZE_t amount = (FSIZE_t)(-(offset + 1)) + 1u;
        if (amount > base)
        {
            errno = EINVAL;
            return (off_t)-1;
        }
        next = base - amount;
    }
    else
    {
        next = base + (FSIZE_t)offset;
    }

    fr = f_lseek(&slot->file, next);
    if (fr != FR_OK)
    {
        mc_fd_set_errno(fr);
        return (off_t)-1;
    }

    return (off_t)f_tell(&slot->file);
}

long _tell(int fd)
{
    off_t pos = _lseek(fd, (off_t)0, SEEK_CUR);
    return (pos < 0) ? -1L : (long)pos;
}

long _filelength(int fd)
{
    mc_fd_file_slot_t *slot = mc_fd_slot_from_fd(fd);

    if (slot == NULL)
    {
        errno = EBADF;
        return -1L;
    }

    return (long)f_size(&slot->file);
}

int _access(const char *path, int mode)
{
    char normalized[MC_FD_PATH_MAX];
    const char *fat_path;
    FILINFO info;
    FRESULT fr;

    (void)mode;

    if (!mc_fd_fs_ensure_ready())
        return -1;

    fat_path = mc_fd_normalize_path(path, normalized);
    if (fat_path == NULL)
        return -1;

    fr = f_stat(fat_path, &info);
    mc_fd_record_path(fat_path, fr);
    if (fr != FR_OK)
    {
        mc_fd_set_errno(fr);
        return -1;
    }

    return 0;
}

int _fstat(int fd, struct stat *st)
{
    mc_fd_file_slot_t *slot;

    if (st == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    memset(st, 0, sizeof(*st));

    if (fd >= 0 && fd < MC_FD_STDIO_COUNT)
    {
        st->st_mode = S_IFCHR;
        return 0;
    }

    slot = mc_fd_slot_from_fd(fd);
    if (slot == NULL)
    {
        errno = EBADF;
        return -1;
    }

    st->st_mode = S_IFREG;
    st->st_size = (off_t)f_size(&slot->file);
    return 0;
}

static int mc_fd_split_pattern(const char *src,
                               char directory[MC_FD_PATH_MAX],
                               char pattern[MC_FD_PATH_MAX])
{
    char full[MC_FD_PATH_MAX];
    const char *normalized;
    char *slash;

    normalized = mc_fd_normalize_path(src, full);
    if (normalized == NULL)
        return 0;

    slash = strrchr(full, '/');
    if (slash == NULL)
    {
        strcpy(directory, "");
        strcpy(pattern, full);
    }
    else
    {
        *slash = '\0';
        strcpy(directory, full);
        strcpy(pattern, slash + 1);
    }

    return 1;
}

static int mc_fd_find_emit(mc_fd_find_slot_t *slot, struct find_t *out)
{
    if (slot->info.fname[0] == '\0')
    {
        (void)f_closedir(&slot->dir);
        slot->used = 0;
        out->handle = NULL;
        errno = ENOENT;
        return 1;
    }

    out->attrib = 0u;
    if (slot->info.fattrib & AM_DIR)
        out->attrib |= _A_SUBDIR;
    if (slot->info.fattrib & AM_ARC)
        out->attrib |= _A_ARCH;

    strncpy(out->name, slot->info.fname, sizeof(out->name) - 1u);
    out->name[sizeof(out->name) - 1u] = '\0';
    out->handle = slot;
    return 0;
}

int _dos_findfirst(const char *pattern,
                   unsigned int attrib,
                   struct find_t *out)
{
    char directory[MC_FD_PATH_MAX];
    char wildcard[MC_FD_PATH_MAX];
    mc_fd_find_slot_t *slot = NULL;
    FRESULT fr;
    unsigned int i;

    (void)attrib;

    if (out == NULL || pattern == NULL)
    {
        errno = EINVAL;
        return 1;
    }

    if (!mc_fd_fs_ensure_ready())
        return 1;

    if (!mc_fd_split_pattern(pattern, directory, wildcard))
        return 1;

    for (i = 0u; i < sizeof(g_mc_fd_finds) / sizeof(g_mc_fd_finds[0]); ++i)
    {
        if (!g_mc_fd_finds[i].used)
        {
            slot = &g_mc_fd_finds[i];
            break;
        }
    }

    if (slot == NULL)
    {
        errno = EMFILE;
        return 1;
    }

    memset(slot, 0, sizeof(*slot));
    slot->used = 1;

    fr = f_findfirst(&slot->dir, &slot->info, directory, wildcard);
    if (fr != FR_OK)
    {
        slot->used = 0;
        mc_fd_set_errno(fr);
        return 1;
    }

    return mc_fd_find_emit(slot, out);
}

int _dos_findnext(struct find_t *out)
{
    mc_fd_find_slot_t *slot;
    FRESULT fr;

    if (out == NULL || out->handle == NULL)
    {
        errno = EINVAL;
        return 1;
    }

    slot = (mc_fd_find_slot_t *)out->handle;
    if (!slot->used)
    {
        errno = EBADF;
        return 1;
    }

    fr = f_findnext(&slot->dir, &slot->info);
    if (fr != FR_OK)
    {
        (void)f_closedir(&slot->dir);
        slot->used = 0;
        out->handle = NULL;
        mc_fd_set_errno(fr);
        return 1;
    }

    return mc_fd_find_emit(slot, out);
}
