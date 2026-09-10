/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Pico 2 / RP2350 I2S transport for the platform-neutral FastDoom ->
 * MicroWave renderer.
 *
 * This deliberately reuses the already-proven MicroConsole Pico audio shape:
 *
 *   - 32 kHz logical audio
 *   - PIO1 SM0 running mw_i2s.pio
 *   - one DMA channel
 *   - two DMA buffers
 *   - IRQ only swaps buffers and requests a refill
 *   - mc_fd_audio_transport_service() performs the expensive Doom/MicroWave
 *     render outside interrupt context
 *
 * The DMA period may be larger than MC_FD_AUDIO_BLOCK. mc_fd_audio_render()
 * handles that by walking the exact same 256-frame Doom/MUS/OPL core used by
 * Raylib and delivering consecutive chunks to the sink below.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "mw_i2s.pio.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"

#include "mc_fastdoom_audio.h"
#include "mc_fastdoom_audio_pico.h"

#ifndef MC_AUDIO_DEVICE
#define MC_AUDIO_DEVICE 1
#endif

#ifndef MC_AUDIO_VOLUME
#define MC_AUDIO_VOLUME 100
#endif

#ifndef MC_I2S_BCLK
#define MC_I2S_BCLK 10
#endif
#ifndef MC_I2S_LRCLK
#define MC_I2S_LRCLK 11
#endif
#ifndef MC_I2S_DATA
#define MC_I2S_DATA 12
#endif

#ifndef MC_FD_PICO_DMA_BLOCK
#define MC_FD_PICO_DMA_BLOCK 1024
#endif

#define MC_FD_PICO_PIO pio1
#define MC_FD_PICO_SM 0

/*
 * The Pico SDK default core-1 stack is only 0x800 (2 KiB).  MicroWave's
 * MUS/MIDI/GENMIDI/Nuked OPL render chain is much deeper than a tiny worker
 * loop, so give the audio core a dedicated 8 KiB SRAM stack instead of relying
 * on the default scratch stack.
 */
#define MC_FD_PICO_CORE1_STACK_BYTES 8192u
static uint32_t __attribute__((aligned(8)))
    mc_fd_pico_core1_stack[MC_FD_PICO_CORE1_STACK_BYTES / sizeof(uint32_t)];

#if MC_I2S_LRCLK != (MC_I2S_BCLK + 1)
#error "MC_I2S_LRCLK must equal MC_I2S_BCLK + 1"
#endif

#if MC_FD_PICO_DMA_BLOCK <= 0
#error "MC_FD_PICO_DMA_BLOCK must be positive"
#endif

#if MC_AUDIO_VOLUME < 0 || MC_AUDIO_VOLUME > 100
#error "MC_AUDIO_VOLUME must be in the range 0..100"
#endif

typedef struct mc_fd_pico_sink {
    uint32_t *dst;
    unsigned int frame_offset;
} mc_fd_pico_sink_t;

static uint32_t mc_fd_pico_i2s[2][MC_FD_PICO_DMA_BLOCK];
static int mc_fd_pico_dma = -1;
static uint mc_fd_pico_pio_offset = 0u;
static volatile int mc_fd_pico_active = 0;
static volatile int mc_fd_pico_ready_buffer[2] = {0, 0};
static volatile int mc_fd_pico_refill = -1;
static volatile unsigned long mc_fd_pico_underrun_count = 0ul;
static volatile unsigned long mc_fd_pico_refill_count = 0ul;
static volatile uint32_t mc_fd_pico_last_refill_us = 0u;
static volatile uint32_t mc_fd_pico_max_refill_us = 0u;
static volatile int mc_fd_pico_core1_started = 0;
static volatile int mc_fd_pico_core1_running = 0;
/*
 * Written by core 0 and polled continuously by core 1.
 * This MUST be volatile (or atomic).  Without that qualifier, -O2 may keep
 * the initial zero in a core-1 register forever because a C data race on a
 * non-volatile object has undefined behavior.
 */
static volatile int mc_fd_pico_ready = 0;
static int mc_fd_pico_failed = 0;
static int mc_fd_pico_pio_claimed = 0;

static int16_t mc_fd_pico_sample_s16(snd_sample_t sample)
{
#if SND_SAMPLE_FORMAT == SND_SAMPLE_FORMAT_U8
    return (int16_t)(((int)sample - 128) << 8);
#else
    return (int16_t)sample;
#endif
}

static uint32_t mc_fd_pico_pack_frame(int16_t left, int16_t right)
{
#if MC_AUDIO_DEVICE == 2
    /* PCM5102A: retain FastDoom's stereo SFX separation. */
    return ((uint32_t)(uint16_t)left << 16) | (uint16_t)right;
#else
    /* MAX98357A and NS4168 are mono output stages. Fold FastDoom's stereo
     * final mix once, then place the same sample in both I2S slots so either
     * channel-selection convention produces the complete game mix. */
    int32_t mono = ((int32_t)left + (int32_t)right) / 2;
    uint16_t sample = (uint16_t)(int16_t)mono;
    return ((uint32_t)sample << 16) | sample;
#endif
}

static void mc_fd_pico_sink(const snd_sample_t *samples,
                            unsigned int frames,
                            void *user)
{
    mc_fd_pico_sink_t *sink = (mc_fd_pico_sink_t *)user;
    unsigned int i;

    if (!sink || !samples)
        return;

    if (sink->frame_offset >= (unsigned int)MC_FD_PICO_DMA_BLOCK)
        return;

    if (frames > (unsigned int)MC_FD_PICO_DMA_BLOCK - sink->frame_offset)
        frames = (unsigned int)MC_FD_PICO_DMA_BLOCK - sink->frame_offset;

    for (i = 0; i < frames; ++i)
    {
        unsigned int src = i * MC_FD_AUDIO_CHANNELS;
        int16_t left = mc_fd_pico_sample_s16(samples[src]);
        int16_t right = mc_fd_pico_sample_s16(samples[src + 1u]);
        sink->dst[sink->frame_offset + i] =
            mc_fd_pico_pack_frame(left, right);
    }

    sink->frame_offset += frames;
}

static void mc_fd_pico_fill(int index)
{
    mc_fd_pico_sink_t sink;

    sink.dst = mc_fd_pico_i2s[index];
    sink.frame_offset = 0u;

    /*
     * Keep the transport period at 1024 frames, but do not hold the shared
     * MicroWave lock for the entire ~27 ms render.  The platform-neutral core
     * already renders internally in 256-frame chunks; invoking it once per
     * chunk releases the lock between chunks so Core 0 can install SFX/music
     * events without stalling behind a whole DMA period.
     */
    while (sink.frame_offset < (unsigned int)MC_FD_PICO_DMA_BLOCK)
    {
        unsigned int remaining =
            (unsigned int)MC_FD_PICO_DMA_BLOCK - sink.frame_offset;
        unsigned int chunk =
            remaining > (unsigned int)MC_FD_AUDIO_BLOCK
                ? (unsigned int)MC_FD_AUDIO_BLOCK
                : remaining;
        unsigned int before = sink.frame_offset;

        mc_fd_audio_render(chunk, mc_fd_pico_sink, &sink);

        if (sink.frame_offset == before)
            break;
    }

    /* Leave any hypothetical short tail as digital silence. */
    while (sink.frame_offset < (unsigned int)MC_FD_PICO_DMA_BLOCK)
        sink.dst[sink.frame_offset++] = 0u;

    __dmb();
    mc_fd_pico_ready_buffer[index] = 1;
}

static void mc_fd_pico_start_dma(int index)
{
    dma_channel_set_read_addr((uint)mc_fd_pico_dma,
                              mc_fd_pico_i2s[index],
                              false);
    dma_channel_set_trans_count((uint)mc_fd_pico_dma,
                                MC_FD_PICO_DMA_BLOCK,
                                true);
}

static void mc_fd_pico_dma_irq(void)
{
    uint32_t mask;
    int done;
    int next;

    if (mc_fd_pico_dma < 0)
        return;

    mask = 1u << (unsigned int)mc_fd_pico_dma;
    if ((dma_hw->ints0 & mask) == 0u)
        return;

    dma_hw->ints0 = mask;

    done = mc_fd_pico_active;
    next = done ^ 1;

    __dmb();
    if (mc_fd_pico_ready_buffer[next])
    {
        mc_fd_pico_ready_buffer[next] = 0;
        mc_fd_pico_active = next;
        mc_fd_pico_start_dma(next);
        mc_fd_pico_refill = done;
    }
    else
    {
        /* Never stop I2S mid-frame. Repeating the just-finished period is a
         * much less destructive failure mode than starving the PIO FIFO. */
        ++mc_fd_pico_underrun_count;
        mc_fd_pico_start_dma(done);
    }
}

int mc_fd_audio_transport_ready(void)
{
    return mc_fd_pico_ready;
}

static void mc_fd_pico_service_once(void)
{
    int index;
    uint32_t begin_us;
    uint32_t elapsed_us;

    if (!mc_fd_pico_ready)
        return;

    index = mc_fd_pico_refill;
    if (index < 0)
        return;

    /* Claim the request before doing expensive MUS/OPL work. */
    mc_fd_pico_refill = -1;
    __dmb();

    begin_us = time_us_32();
    mc_fd_pico_fill(index);
    elapsed_us = time_us_32() - begin_us;

    mc_fd_pico_last_refill_us = elapsed_us;
    if (elapsed_us > mc_fd_pico_max_refill_us)
        mc_fd_pico_max_refill_us = elapsed_us;

    ++mc_fd_pico_refill_count;
}

static void mc_fd_pico_core1_main(void)
{
    mc_fd_pico_core1_running = 1;
    __dmb();

    for (;;)
    {
        if (mc_fd_pico_ready && mc_fd_pico_refill >= 0)
            mc_fd_pico_service_once();
        else
            tight_loop_contents();
    }
}

void mc_fd_audio_transport_service(void)
{
    /*
     * Once core 1 is dedicated to audio, all historical service calls made by
     * Doom/core 0 become intentionally cheap no-ops.  Before core 1 exists,
     * retain the old synchronous behavior for startup/failure safety.
     */
    if (mc_fd_pico_core1_started && get_core_num() != 1u)
        return;

    mc_fd_pico_service_once();
}

int mc_fd_audio_transport_start(void)
{
    uint32_t sys_hz;
    uint32_t div256;
    dma_channel_config dc;
    int dma;

    if (mc_fd_pico_ready)
        return 1;

    /* Explicit -nosound: there is deliberately no audio core to service. */
    if (!mc_fd_audio_core_ready())
        return 1;

    if (mc_fd_pico_failed)
        return 0;

    /* The console-wide control uses MicroWave's own final output-stage volume
     * rather than scaling I2S words in this transport. This is the same path a
     * future ADC/potentiometer should drive. */
    mc_fd_audio_set_master_volume(snd_vol_from_percent(MC_AUDIO_VOLUME));

    gpio_set_function(MC_I2S_DATA, GPIO_FUNC_PIO1);
    gpio_set_function(MC_I2S_BCLK, GPIO_FUNC_PIO1);
    gpio_set_function(MC_I2S_LRCLK, GPIO_FUNC_PIO1);

    pio_sm_claim(MC_FD_PICO_PIO, MC_FD_PICO_SM);
    mc_fd_pico_pio_claimed = 1;
    mc_fd_pico_pio_offset =
        pio_add_program(MC_FD_PICO_PIO, &mw_i2s_program);
    mw_i2s_program_init(MC_FD_PICO_PIO,
                        MC_FD_PICO_SM,
                        mc_fd_pico_pio_offset,
                        MC_I2S_DATA,
                        MC_I2S_BCLK);

    sys_hz = clock_get_hz(clk_sys);
    div256 = (uint32_t)((((uint64_t)sys_hz * 4u) +
                         (uint32_t)MC_FD_AUDIO_RATE / 2u) /
                        (uint32_t)MC_FD_AUDIO_RATE);
    pio_sm_set_clkdiv_int_frac(MC_FD_PICO_PIO,
                               MC_FD_PICO_SM,
                               (uint16_t)(div256 >> 8u),
                               (uint8_t)(div256 & 0xffu));

    dma = dma_claim_unused_channel(false);
    if (dma < 0)
    {
        fprintf(stderr, "MicroWave: Pico I2S has no free DMA channel\n");
        pio_sm_unclaim(MC_FD_PICO_PIO, MC_FD_PICO_SM);
        mc_fd_pico_pio_claimed = 0;
        pio_remove_program(MC_FD_PICO_PIO,
                           &mw_i2s_program,
                           mc_fd_pico_pio_offset);
        mc_fd_pico_failed = 1;
        mc_fd_audio_transport_failed();
        return 0;
    }

    mc_fd_pico_dma = dma;
    dc = dma_channel_get_default_config((uint)mc_fd_pico_dma);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_read_increment(&dc, true);
    channel_config_set_write_increment(&dc, false);
    channel_config_set_dreq(&dc, DREQ_PIO1_TX0 + MC_FD_PICO_SM);
    dma_channel_configure((uint)mc_fd_pico_dma,
                          &dc,
                          &MC_FD_PICO_PIO->txf[MC_FD_PICO_SM],
                          NULL,
                          0,
                          false);

    memset(mc_fd_pico_i2s, 0, sizeof(mc_fd_pico_i2s));
    mc_fd_pico_active = 0;
    mc_fd_pico_ready_buffer[0] = 0;
    mc_fd_pico_ready_buffer[1] = 0;
    mc_fd_pico_refill = 1;
    mc_fd_pico_underrun_count = 0ul;
    mc_fd_pico_refill_count = 0ul;
    mc_fd_pico_last_refill_us = 0u;
    mc_fd_pico_max_refill_us = 0u;

    if (!mc_fd_pico_core1_started)
    {
        memset(mc_fd_pico_core1_stack, 0xa5,
               sizeof(mc_fd_pico_core1_stack));

        multicore_launch_core1_with_stack(
            mc_fd_pico_core1_main,
            mc_fd_pico_core1_stack,
            sizeof(mc_fd_pico_core1_stack));

        mc_fd_pico_core1_started = 1;
        __dmb();

        while (!mc_fd_pico_core1_running)
            tight_loop_contents();
    }

    irq_set_exclusive_handler(DMA_IRQ_0, mc_fd_pico_dma_irq);
    dma_channel_set_irq0_enabled((uint)mc_fd_pico_dma, true);
    irq_set_enabled(DMA_IRQ_0, true);
    pio_sm_set_enabled(MC_FD_PICO_PIO, MC_FD_PICO_SM, true);

    /* Start with one zeroed period. S_StartSound()/S_ChangeMusic() call the
     * service hook after installing the first event, so buffer 1 is rendered
     * with real Doom state rather than pre-advancing 32 ms of silence before
     * the event exists. */
    mc_fd_pico_ready = 1;
    mc_fd_pico_start_dma(0);

    printf("MicroWave FastDoom Pico: %d Hz I2S DMA, block=%d, "
           "BCLK=%d LRCLK=%d DATA=%d, volume=%d%%\n",
           MC_FD_AUDIO_RATE,
           MC_FD_PICO_DMA_BLOCK,
           MC_I2S_BCLK,
           MC_I2S_LRCLK,
           MC_I2S_DATA,
           MC_AUDIO_VOLUME);
    fflush(stdout);

    return 1;
}

void mc_fd_audio_transport_stop(void)
{
    if (mc_fd_pico_dma >= 0)
    {
        uint32_t mask = 1u << (unsigned int)mc_fd_pico_dma;

        dma_channel_set_irq0_enabled((uint)mc_fd_pico_dma, false);
        dma_channel_abort((uint)mc_fd_pico_dma);
        dma_hw->ints0 = mask;
        dma_channel_unclaim((uint)mc_fd_pico_dma);
        mc_fd_pico_dma = -1;
    }

    if (mc_fd_pico_pio_claimed)
    {
        pio_sm_set_enabled(MC_FD_PICO_PIO, MC_FD_PICO_SM, false);
        pio_sm_clear_fifos(MC_FD_PICO_PIO, MC_FD_PICO_SM);
        pio_sm_unclaim(MC_FD_PICO_PIO, MC_FD_PICO_SM);
        pio_remove_program(MC_FD_PICO_PIO,
                           &mw_i2s_program,
                           mc_fd_pico_pio_offset);
        mc_fd_pico_pio_claimed = 0;
    }

    mc_fd_pico_ready = 0;
    mc_fd_pico_failed = 0;
    mc_fd_pico_active = 0;
    mc_fd_pico_ready_buffer[0] = 0;
    mc_fd_pico_ready_buffer[1] = 0;
    mc_fd_pico_refill = -1;
}

unsigned long mc_fd_audio_pico_underruns(void)
{
    return mc_fd_pico_underrun_count;
}

unsigned long mc_fd_audio_pico_refills(void)
{
    return mc_fd_pico_refill_count;
}

int mc_fd_audio_pico_core1_running(void)
{
    return mc_fd_pico_core1_running ? 1 : 0;
}

uint32_t mc_fd_audio_pico_last_refill_us(void)
{
    return mc_fd_pico_last_refill_us;
}

uint32_t mc_fd_audio_pico_max_refill_us(void)
{
    return mc_fd_pico_max_refill_us;
}

unsigned int mc_fd_audio_pico_core1_stack_bytes(void)
{
    return (unsigned int)sizeof(mc_fd_pico_core1_stack);
}
