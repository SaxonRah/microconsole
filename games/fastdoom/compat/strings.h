/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MC_FASTDOOM_COMPAT_STRINGS_H
#define MC_FASTDOOM_COMPAT_STRINGS_H

#if defined(MC_FASTDOOM_PICO)

#include <stddef.h>

/*
 * Some newlib feature configurations do not expose strcasecmp declarations
 * from <strings.h>. FastDoom only needs ASCII command/file-name comparison,
 * so keep the Pico implementation header-local and deterministic.
 */
static inline unsigned char mc_fd_ascii_lower(unsigned char c)
{
    if (c >= (unsigned char)'A' && c <= (unsigned char)'Z')
        c = (unsigned char)(c + ((unsigned char)'a' - (unsigned char)'A'));
    return c;
}

static inline int mc_fd_strcasecmp(const char *a, const char *b)
{
    unsigned char ca;
    unsigned char cb;

    do
    {
        ca = mc_fd_ascii_lower((unsigned char)*a++);
        cb = mc_fd_ascii_lower((unsigned char)*b++);

        if (ca != cb)
            return (ca < cb) ? -1 : 1;
    } while (ca != 0u);

    return 0;
}

static inline int mc_fd_strncasecmp(const char *a, const char *b, size_t n)
{
    unsigned char ca;
    unsigned char cb;

    while (n != 0u)
    {
        ca = mc_fd_ascii_lower((unsigned char)*a++);
        cb = mc_fd_ascii_lower((unsigned char)*b++);

        if (ca != cb)
            return (ca < cb) ? -1 : 1;
        if (ca == 0u)
            return 0;

        --n;
    }

    return 0;
}

#define strcasecmp  mc_fd_strcasecmp
#define strncasecmp mc_fd_strncasecmp
#define strcmpi     mc_fd_strcasecmp

#elif defined(_MSC_VER)

#include <string.h>
#define strcasecmp  _stricmp
#define strncasecmp _strnicmp
#define strcmpi     _stricmp

#else

#include_next <strings.h>
#ifndef strcmpi
#define strcmpi strcasecmp
#endif

#endif

#endif
