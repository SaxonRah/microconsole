/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MicroConsole FastDoom PSRAM milestone.
 *
 * The Pimoroni Pico Plus 2 has enough external PSRAM for FastDoom's zone, but
 * Doom must not be allowed to depend on it until the exact board/toolchain
 * combination proves that the memory is initialized and writable.
 *
 * This target is intentionally independent of FastDoom itself. It reserves a
 * 6 MiB uninitialized PSRAM region (so the UF2 is not inflated), verifies that
 * hardware_psram recognizes the allocation, then writes and reads one word per
 * 4 KiB page across the entire region.
 */

#include "hardware/psram.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef MC_FD_PSRAM_ZONE_BYTES
#define MC_FD_PSRAM_ZONE_BYTES (6u * 1024u * 1024u)
#endif

#define MC_FD_PROBE_PAGE_BYTES 4096u
#define MC_FD_PROBE_WORDS (MC_FD_PSRAM_ZONE_BYTES / sizeof(uint32_t))

_Static_assert(
    (MC_FD_PSRAM_ZONE_BYTES % sizeof(uint32_t)) == 0u,
    "MC_FD_PSRAM_ZONE_BYTES must be a multiple of uint32_t");

static uint32_t __uninitialized_psram("mc_fd_zone_probe")
    g_mc_fd_zone[MC_FD_PROBE_WORDS];

static int g_probe_ok;
static size_t g_psram_bytes;
static unsigned int g_pages_tested;

static uint32_t mc_pattern(unsigned int page)
{
    uint32_t x = 0x4D434644u; /* "MCFD" */
    x ^= (uint32_t)page * 0x9E3779B9u;
    x ^= x >> 16;
    return x;
}

static int mc_test_zone(void)
{
    volatile uint32_t *zone = g_mc_fd_zone;
    unsigned int page_count =
        (unsigned int)(MC_FD_PSRAM_ZONE_BYTES / MC_FD_PROBE_PAGE_BYTES);
    unsigned int words_per_page =
        (unsigned int)(MC_FD_PROBE_PAGE_BYTES / sizeof(uint32_t));
    unsigned int page;

    g_pages_tested = 0u;

    for (page = 0u; page < page_count; ++page)
    {
        unsigned int index = page * words_per_page;
        zone[index] = mc_pattern(page);
    }

    __dmb();

    for (page = 0u; page < page_count; ++page)
    {
        unsigned int index = page * words_per_page;
        uint32_t expected = mc_pattern(page);
        uint32_t actual = zone[index];

        if (actual != expected)
        {
            printf("MCFDPSRAM1 error=verify page=%u offset=%u "
                   "expected=%08lx actual=%08lx\n",
                   page,
                   page * MC_FD_PROBE_PAGE_BYTES,
                   (unsigned long)expected,
                   (unsigned long)actual);
            fflush(stdout);
            return 0;
        }

        ++g_pages_tested;
    }

    /*
     * Explicitly touch the final word as well. The 4 KiB page walk touches the
     * beginning of the final page; this separately catches a bad upper bound.
     */
    zone[MC_FD_PROBE_WORDS - 1u] = 0xA55A5AA5u;
    __dmb();
    if (zone[MC_FD_PROBE_WORDS - 1u] != 0xA55A5AA5u)
    {
        printf("MCFDPSRAM1 error=verify-tail\n");
        fflush(stdout);
        return 0;
    }

    return 1;
}

static void mc_report(void)
{
    printf("MCFDPSRAM1 available=%d psram=%lu zone=%lu pages=%u "
           "start=%p end=%p ok=%d\n",
           psram_is_available() ? 1 : 0,
           (unsigned long)g_psram_bytes,
           (unsigned long)MC_FD_PSRAM_ZONE_BYTES,
           g_pages_tested,
           (void *)&g_mc_fd_zone[0],
           (void *)&g_mc_fd_zone[MC_FD_PROBE_WORDS - 1u],
           g_probe_ok);
    fflush(stdout);
}

static void mc_serial_service(void)
{
    static char cmd[16];
    static unsigned int count;
    int ch;

    while ((ch = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT)
    {
        if (ch == '\r' || ch == '\n')
        {
            if (count != 0u)
            {
                cmd[count] = '\0';
                if (strcmp(cmd, "PING") == 0)
                    mc_report();
                count = 0u;
            }
        }
        else if (count + 1u < sizeof(cmd))
        {
            cmd[count++] = (char)ch;
        }
        else
        {
            count = 0u;
        }
    }
}

int main(void)
{
    stdio_init_all();
    sleep_ms(250);

    g_psram_bytes = psram_get_size();

    printf("MCFDPSRAM1 boot sdk-psram=%d bytes=%lu zone=%lu\n",
           psram_is_available() ? 1 : 0,
           (unsigned long)g_psram_bytes,
           (unsigned long)MC_FD_PSRAM_ZONE_BYTES);
    fflush(stdout);

    if (!psram_is_available())
    {
        printf("MCFDPSRAM1 error=psram-unavailable\n");
        fflush(stdout);
    }
    else if (g_psram_bytes < (size_t)MC_FD_PSRAM_ZONE_BYTES)
    {
        printf("MCFDPSRAM1 error=psram-too-small bytes=%lu need=%lu\n",
               (unsigned long)g_psram_bytes,
               (unsigned long)MC_FD_PSRAM_ZONE_BYTES);
        fflush(stdout);
    }
    else if (!psram_check_address((void *)&g_mc_fd_zone[0]) ||
             !psram_check_address(
                 (void *)&g_mc_fd_zone[MC_FD_PROBE_WORDS - 1u]))
    {
        printf("MCFDPSRAM1 error=zone-not-in-psram start=%p end=%p\n",
               (void *)&g_mc_fd_zone[0],
               (void *)&g_mc_fd_zone[MC_FD_PROBE_WORDS - 1u]);
        fflush(stdout);
    }
    else
    {
        g_probe_ok = mc_test_zone();
    }

    mc_report();

    for (;;)
    {
        mc_serial_service();
        sleep_ms(1);
    }
}
