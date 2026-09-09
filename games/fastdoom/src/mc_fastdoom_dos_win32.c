/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Win32-only compatibility helpers for the MicroConsole FastDoom Raylib
 * target. Keep Windows SDK headers isolated here so they cannot collide
 * with FastDoom's boolean typedef or MIN/MAX macros.
 */

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <string.h>

#include "dos.h"

static int mc_symbols_ready = 0;

static void mc_fastdoom_find_copy(struct find_t *out,
                                  const WIN32_FIND_DATAA *data)
{
    out->attrib = 0;

    if ((data->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        out->attrib |= _A_SUBDIR;
    if ((data->dwFileAttributes & FILE_ATTRIBUTE_ARCHIVE) != 0)
        out->attrib |= _A_ARCH;

    strncpy(out->name, data->cFileName, sizeof(out->name) - 1u);
    out->name[sizeof(out->name) - 1u] = '\0';
}

int _dos_findfirst(const char *pattern, unsigned int attrib, struct find_t *out)
{
    WIN32_FIND_DATAA data;
    HANDLE handle;

    (void)attrib;

    if (out == NULL)
        return 1;

    out->handle = NULL;
    out->attrib = 0;
    out->name[0] = '\0';

    handle = FindFirstFileA(pattern, &data);
    if (handle == INVALID_HANDLE_VALUE)
        return 1;

    out->handle = (void *)handle;
    mc_fastdoom_find_copy(out, &data);
    return 0;
}

int _dos_findnext(struct find_t *out)
{
    WIN32_FIND_DATAA data;
    HANDLE handle;

    if (out == NULL || out->handle == NULL)
        return 1;

    handle = (HANDLE)out->handle;

    if (!FindNextFileA(handle, &data))
    {
        FindClose(handle);
        out->handle = NULL;
        return 1;
    }

    mc_fastdoom_find_copy(out, &data);
    return 0;
}

static void mc_fastdoom_print_symbol(void *address)
{
    HANDLE process = GetCurrentProcess();
    DWORD64 addr = (DWORD64)(ULONG_PTR)address;
    DWORD64 displacement = 0;
    DWORD line_displacement = 0;
    unsigned char symbol_buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME + 1];
    SYMBOL_INFO *symbol = (SYMBOL_INFO *)symbol_buffer;
    IMAGEHLP_LINE64 line;

    if (!mc_symbols_ready || address == NULL)
        return;

    memset(symbol_buffer, 0, sizeof(symbol_buffer));
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;

    if (SymFromAddr(process, addr, &displacement, symbol))
    {
        fprintf(stderr,
                "MicroConsole FastDoom: symbol %s+0x%llX\n",
                symbol->Name,
                (unsigned long long)displacement);
    }

    memset(&line, 0, sizeof(line));
    line.SizeOfStruct = sizeof(line);
    if (SymGetLineFromAddr64(process, addr, &line_displacement, &line))
    {
        fprintf(stderr,
                "MicroConsole FastDoom: source %s:%lu (+0x%lX)\n",
                line.FileName ? line.FileName : "<unknown>",
                (unsigned long)line.LineNumber,
                (unsigned long)line_displacement);
    }
}

static LONG WINAPI mc_fastdoom_exception_filter(EXCEPTION_POINTERS *info)
{
    unsigned long code = 0;
    void *address = NULL;

    if (info != NULL && info->ExceptionRecord != NULL)
    {
        code = (unsigned long)info->ExceptionRecord->ExceptionCode;
        address = info->ExceptionRecord->ExceptionAddress;
    }

    fprintf(stderr,
            "\nMicroConsole FastDoom: unhandled Windows exception "
            "0x%08lX at %p\n",
            code,
            address);

#if defined(_M_IX86)
    if (info != NULL && info->ContextRecord != NULL)
    {
        CONTEXT *ctx = info->ContextRecord;
        fprintf(stderr,
                "MicroConsole FastDoom: EIP=%08lX ESP=%08lX EBP=%08lX "
                "EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX\n",
                (unsigned long)ctx->Eip,
                (unsigned long)ctx->Esp,
                (unsigned long)ctx->Ebp,
                (unsigned long)ctx->Eax,
                (unsigned long)ctx->Ebx,
                (unsigned long)ctx->Ecx,
                (unsigned long)ctx->Edx);
    }
#endif

    mc_fastdoom_print_symbol(address);
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

void MC_FastDoomInstallCrashHandler(void)
{
    HANDLE process = GetCurrentProcess();

    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
    if (SymInitialize(process, NULL, TRUE))
        mc_symbols_ready = 1;
    else
        fprintf(stderr,
                "MicroConsole FastDoom: warning: DbgHelp symbol initialization "
                "failed (%lu).\n",
                (unsigned long)GetLastError());

    SetUnhandledExceptionFilter(mc_fastdoom_exception_filter);
}
