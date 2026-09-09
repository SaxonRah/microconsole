/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MC_FASTDOOM_COMPAT_IO_H
#define MC_FASTDOOM_COMPAT_IO_H

#include <fcntl.h>
#include <sys/stat.h>

#if defined(_MSC_VER)

#ifdef __cplusplus
extern "C" {
#endif

int  __cdecl _open(const char *filename, int oflag, ...);
int  __cdecl _close(int fd);
int  __cdecl _read(int fd, void *buffer, unsigned int count);
int  __cdecl _write(int fd, const void *buffer, unsigned int count);
long __cdecl _lseek(int fd, long offset, int origin);
long __cdecl _tell(int fd);
long __cdecl _filelength(int fd);
int  __cdecl _access(const char *path, int mode);

#ifdef __cplusplus
}
#endif

/*
 * IMPORTANT: do not macro-map open/close/read/etc. here. FastDoom has enum
 * values named "open" and "close" in p_spec.h; global CRT-name macros turn
 * those enum constants into _open/_close and collide with MSVC declarations.
 * The generated source overlay rewrites only actual function-call sites.
 */
#ifndef O_BINARY
#define O_BINARY _O_BINARY
#endif

#else
#include_next <io.h>
#endif

#endif
