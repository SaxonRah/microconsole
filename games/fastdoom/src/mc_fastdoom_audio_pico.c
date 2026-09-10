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
 *   - four short queued DMA periods
 *   - Core 1 owns the producer and MUS/MIDI/GENMIDI/Nuked transitions
 *   - IRQ only retires/starts already-rendered periods
 *
 * The producer renders 512-frame transport periods as two consecutive
 * 256-frame authoritative Doom/MUS/OPL chunks.  Keeping two periods ready
 * absorbs normal synthesis jitter without turning an underrun into repeated,
 * slow-sounding game audio.
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
#define MC_FD_PICO_DMA_BLOCK 512
#endif

#ifndef MC_FD_PICO_RING_COUNT
#define MC_FD_PICO_RING_COUNT 4
#endif

#ifndef MC_FD_PICO_RING_TARGET
#define MC_FD_PICO_RING_TARGET 2
#endif

#define MC_FD_PICO_TIMING_WINDOW 32u

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

#if MC_FD_PICO_RING_COUNT < 2
#error "MC_FD_PICO_RING_COUNT must be at least 2"
#endif

#if MC_FD_PICO_RING_TARGET < 1 || MC_FD_PICO_RING_TARGET >= MC_FD_PICO_RING_COUNT
#error "MC_FD_PICO_RING_TARGET must be 1..MC_FD_PICO_RING_COUNT-1"
#endif

#if MC_AUDIO_VOLUME < 0 || MC_AUDIO_VOLUME > 100
#error "MC_AUDIO_VOLUME must be in the range 0..100"
#endif

typedef struct mc_fd_pico_sink {
    uint32_t *dst;
    unsigned int frame_offset;
} mc_fd_pico_sink_t;

enum {
    MC_FD_RING_FREE = 0,
    MC_FD_RING_READY = 1,
    MC_FD_RING_ACTIVE = 2
};

static uint32_t
    mc_fd_pico_i2s[MC_FD_PICO_RING_COUNT][MC_FD_PICO_DMA_BLOCK];
static uint32_t mc_fd_pico_silence[MC_FD_PICO_DMA_BLOCK];
static volatile uint8_t mc_fd_pico_ring_state[MC_FD_PICO_RING_COUNT];
static volatile unsigned int mc_fd_pico_ring_write = 0u;
static volatile unsigned int mc_fd_pico_ring_read = 0u;
static volatile int mc_fd_pico_active = -1;

static int mc_fd_pico_dma = -1;
static uint mc_fd_pico_pio_offset = 0u;

static volatile unsigned long mc_fd_pico_underrun_count = 0ul;
static volatile unsigned long mc_fd_pico_refill_count = 0ul;
static volatile uint32_t mc_fd_pico_last_refill_us = 0u;
static volatile uint32_t mc_fd_pico_min_refill_us = 0u;
static volatile uint32_t mc_fd_pico_avg_refill_us = 0u;
static volatile uint32_t mc_fd_pico_max_refill_us = 0u;
static volatile unsigned int mc_fd_pico_ring_low_water =
    MC_FD_PICO_RING_TARGET;
static volatile unsigned int mc_fd_pico_ring_high_water = 0u;

static uint32_t mc_fd_pico_timing_samples[MC_FD_PICO_TIMING_WINDOW];
static uint32_t mc_fd_pico_timing_sum;
static unsigned int mc_fd_pico_timing_count;
static unsigned int mc_fd_pico_timing_pos;

static volatile int mc_fd_pico_core1_started = 0;
static volatile int mc_fd_pico_core1_running = 0;

/*
 * Core-0 -> Core-1 control mailbox.
 *
 * A monotonically increasing request sequence makes every transaction unique.
 * The callback executes only on Core 1, between complete 256-frame
 * MicroWave renders, so MUS/MIDI/GENMIDI/Nuked have one owner.
 */
static mc_fd_audio_pico_control_fn volatile mc_fd_pico_control_fn_ptr = NULL;
static void * volatile mc_fd_pico_control_user = NULL;
static volatile uint32_t mc_fd_pico_control_request_seq = 0u;
static volatile uint32_t mc_fd_pico_control_done_seq = 0u;
static volatile unsigned long mc_fd_pico_control_timeout_count = 0ul;

static volatile int mc_fd_pico_ready = 0;
static int mc_fd_pico_failed = 0;
static int mc_fd_pico_pio_claimed = 0;

#if defined(__GNUC__)
#define MC_FD_PICO_HOT(name) \
    __attribute__((noinline, section(".time_critical." #name))) name
#else
#define MC_FD_PICO_HOT(name) name
#endif

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
    return ((uint32_t)(uint16_t)left << 16) | (uint16_t)right;
#else
    int32_t mono = ((int32_t)left + (int32_t)right) / 2;
    uint16_t sample = (uint16_t)(int16_t)mono;
    return ((uint32_t)sample << 16) | sample;
#endif
}

static unsigned int mc_fd_pico_ring_ready_count_snapshot(void)
{
    unsigned int i;
    unsigned int count = 0u;

    for (i = 0u; i < (unsigned int)MC_FD_PICO_RING_COUNT; ++i)
    {
        if (mc_fd_pico_ring_state[i] == MC_FD_RING_READY)
            ++count;
    }

    return count;
}

static void MC_FD_PICO_HOT(mc_fd_pico_sink)(const snd_sample_t *samples,
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

static void mc_fd_pico_control_checkpoint(void);

static void MC_FD_PICO_HOT(mc_fd_pico_fill)(int index)
{
    mc_fd_pico_sink_t sink;

    sink.dst = mc_fd_pico_i2s[index];
    sink.frame_offset = 0u;

    /*
     * A transport period is 512 frames, but the authoritative Doom audio core
     * remains 256 frames.  Releasing the core lock and checking the control
     * mailbox between chunks keeps music transitions responsive.
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
        mc_fd_pico_control_checkpoint();

        if (sink.frame_offset == before)
            break;
    }

    while (sink.frame_offset < (unsigned int)MC_FD_PICO_DMA_BLOCK)
        sink.dst[sink.frame_offset++] = 0u;
}

static void mc_fd_pico_start_dma_buffer(const uint32_t *buffer)
{
    dma_channel_set_read_addr((uint)mc_fd_pico_dma, buffer, false);
    dma_channel_set_trans_count((uint)mc_fd_pico_dma,
                                MC_FD_PICO_DMA_BLOCK,
                                true);
}

static void mc_fd_pico_dma_irq(void)
{
    uint32_t mask;
    unsigned int ready_after;

    if (mc_fd_pico_dma < 0)
        return;

    mask = 1u << (unsigned int)mc_fd_pico_dma;
    if ((dma_hw->ints0 & mask) == 0u)
        return;

    dma_hw->ints0 = mask;

    if (mc_fd_pico_active >= 0)
    {
        mc_fd_pico_ring_state[mc_fd_pico_active] = MC_FD_RING_FREE;
        mc_fd_pico_active = -1;
        __dmb();
    }

    if (mc_fd_pico_ring_state[mc_fd_pico_ring_read] == MC_FD_RING_READY)
    {
        unsigned int index = mc_fd_pico_ring_read;

        mc_fd_pico_ring_state[index] = MC_FD_RING_ACTIVE;
        mc_fd_pico_active = (int)index;
        mc_fd_pico_ring_read =
            (index + 1u) % (unsigned int)MC_FD_PICO_RING_COUNT;
        __dmb();

        mc_fd_pico_start_dma_buffer(mc_fd_pico_i2s[index]);
    }
    else
    {
        /*
         * Preserve real audio time on a miss.  Repeating the previous period
         * made effects sound slow; one zero period is a short dropout instead.
         */
        ++mc_fd_pico_underrun_count;
        mc_fd_pico_start_dma_buffer(mc_fd_pico_silence);
    }

    ready_after = mc_fd_pico_ring_ready_count_snapshot();
    if (ready_after < mc_fd_pico_ring_low_water)
        mc_fd_pico_ring_low_water = ready_after;
}

int mc_fd_audio_transport_ready(void)
{
    return mc_fd_pico_ready;
}

static void mc_fd_pico_update_timing(uint32_t elapsed_us)
{
    uint32_t old;

    old = mc_fd_pico_timing_samples[mc_fd_pico_timing_pos];
    mc_fd_pico_timing_sum -= old;
    mc_fd_pico_timing_samples[mc_fd_pico_timing_pos] = elapsed_us;
    mc_fd_pico_timing_sum += elapsed_us;

    mc_fd_pico_timing_pos =
        (mc_fd_pico_timing_pos + 1u) % MC_FD_PICO_TIMING_WINDOW;

    if (mc_fd_pico_timing_count < MC_FD_PICO_TIMING_WINDOW)
        ++mc_fd_pico_timing_count;

    if (mc_fd_pico_timing_count != 0u)
        mc_fd_pico_avg_refill_us =
            mc_fd_pico_timing_sum / mc_fd_pico_timing_count;
}

static void MC_FD_PICO_HOT(mc_fd_pico_service_once)(void)
{
    unsigned int index;
    unsigned int ready;
    uint32_t begin_us;
    uint32_t elapsed_us;

    if (!mc_fd_pico_ready)
        return;

    ready = mc_fd_pico_ring_ready_count_snapshot();
    if (ready >= (unsigned int)MC_FD_PICO_RING_TARGET)
        return;

    index = mc_fd_pico_ring_write;
    if (mc_fd_pico_ring_state[index] != MC_FD_RING_FREE)
        return;

    begin_us = time_us_32();
    mc_fd_pico_fill((int)index);
    elapsed_us = time_us_32() - begin_us;

    __dmb();
    mc_fd_pico_ring_state[index] = MC_FD_RING_READY;
    mc_fd_pico_ring_write =
        (index + 1u) % (unsigned int)MC_FD_PICO_RING_COUNT;
    __dmb();

    mc_fd_pico_last_refill_us = elapsed_us;
    if (mc_fd_pico_min_refill_us == 0u ||
        elapsed_us < mc_fd_pico_min_refill_us)
        mc_fd_pico_min_refill_us = elapsed_us;
    if (elapsed_us > mc_fd_pico_max_refill_us)
        mc_fd_pico_max_refill_us = elapsed_us;
    mc_fd_pico_update_timing(elapsed_us);

    ++mc_fd_pico_refill_count;

    ready = mc_fd_pico_ring_ready_count_snapshot();
    if (ready > mc_fd_pico_ring_high_water)
        mc_fd_pico_ring_high_water = ready;
}

static void mc_fd_pico_control_checkpoint(void)
{
    uint32_t request;
    mc_fd_audio_pico_control_fn fn;
    void *user;

    request = mc_fd_pico_control_request_seq;
    if (request == mc_fd_pico_control_done_seq)
        return;

    __dmb();
    fn = mc_fd_pico_control_fn_ptr;
    user = mc_fd_pico_control_user;

    if (fn)
        fn(user);

    __dmb();
    mc_fd_pico_control_done_seq = request;
    __dmb();
}

static void mc_fd_pico_core1_main(void)
{
    mc_fd_pico_core1_running = 1;
    __dmb();

    for (;;)
    {
        mc_fd_pico_control_checkpoint();

        if (mc_fd_pico_ready &&
            mc_fd_pico_ring_ready_count_snapshot() <
                (unsigned int)MC_FD_PICO_RING_TARGET)
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
    memset(mc_fd_pico_silence, 0, sizeof(mc_fd_pico_silence));
    memset((void *)mc_fd_pico_ring_state, 0, sizeof(mc_fd_pico_ring_state));
    memset(mc_fd_pico_timing_samples, 0, sizeof(mc_fd_pico_timing_samples));
    mc_fd_pico_ring_write = 0u;
    mc_fd_pico_ring_read = 0u;
    mc_fd_pico_active = -1;
    mc_fd_pico_underrun_count = 0ul;
    mc_fd_pico_refill_count = 0ul;
    mc_fd_pico_last_refill_us = 0u;
    mc_fd_pico_min_refill_us = 0u;
    mc_fd_pico_avg_refill_us = 0u;
    mc_fd_pico_max_refill_us = 0u;
    mc_fd_pico_ring_low_water = MC_FD_PICO_RING_TARGET;
    mc_fd_pico_ring_high_water = 0u;
    mc_fd_pico_timing_sum = 0u;
    mc_fd_pico_timing_count = 0u;
    mc_fd_pico_timing_pos = 0u;
    mc_fd_pico_control_fn_ptr = NULL;
    mc_fd_pico_control_user = NULL;
    mc_fd_pico_control_request_seq = 0u;
    mc_fd_pico_control_done_seq = 0u;
    mc_fd_pico_control_timeout_count = 0ul;

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

    /*
     * Start one short zero period while Core 1 fills the producer ring.
     * The first real slot can be consumed on the next 16 ms DMA boundary.
     */
    mc_fd_pico_ready = 1;
    mc_fd_pico_start_dma_buffer(mc_fd_pico_silence);

    printf("MicroWave FastDoom Pico: %d Hz I2S DMA, block=%d ring=%d ahead=%d, "
           "BCLK=%d LRCLK=%d DATA=%d, volume=%d%%\n",
           MC_FD_AUDIO_RATE,
           MC_FD_PICO_DMA_BLOCK,
           MC_FD_PICO_RING_COUNT,
           MC_FD_PICO_RING_TARGET,
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
    mc_fd_pico_active = -1;
    memset((void *)mc_fd_pico_ring_state, 0, sizeof(mc_fd_pico_ring_state));
    mc_fd_pico_ring_write = 0u;
    mc_fd_pico_ring_read = 0u;
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

uint32_t mc_fd_audio_pico_min_refill_us(void)
{
    return mc_fd_pico_min_refill_us;
}

uint32_t mc_fd_audio_pico_avg_refill_us(void)
{
    return mc_fd_pico_avg_refill_us;
}

uint32_t mc_fd_audio_pico_max_refill_us(void)
{
    return mc_fd_pico_max_refill_us;
}

unsigned int mc_fd_audio_pico_block_frames(void)
{
    return (unsigned int)MC_FD_PICO_DMA_BLOCK;
}

unsigned int mc_fd_audio_pico_period_us(void)
{
    return (unsigned int)(
        (((uint64_t)MC_FD_PICO_DMA_BLOCK * 1000000ull) +
         ((uint64_t)MC_FD_AUDIO_RATE / 2ull)) /
        (uint64_t)MC_FD_AUDIO_RATE);
}

unsigned int mc_fd_audio_pico_ring_ready(void)
{
    return mc_fd_pico_ring_ready_count_snapshot();
}

unsigned int mc_fd_audio_pico_ring_count(void)
{
    return (unsigned int)MC_FD_PICO_RING_COUNT;
}

unsigned int mc_fd_audio_pico_ring_target(void)
{
    return (unsigned int)MC_FD_PICO_RING_TARGET;
}

unsigned int mc_fd_audio_pico_ring_low_water(void)
{
    return mc_fd_pico_ring_low_water;
}

unsigned int mc_fd_audio_pico_ring_high_water(void)
{
    return mc_fd_pico_ring_high_water;
}

unsigned int mc_fd_audio_pico_core1_stack_bytes(void)
{
    return (unsigned int)sizeof(mc_fd_pico_core1_stack);
}

unsigned int mc_fd_audio_pico_core1_stack_used_bytes(void)
{
    const uint32_t fill = 0xa5a5a5a5u;
    unsigned int words =
        (unsigned int)(sizeof(mc_fd_pico_core1_stack) /
                       sizeof(mc_fd_pico_core1_stack[0]));
    unsigned int i;

    for (i = 0u; i < words; ++i)
    {
        if (mc_fd_pico_core1_stack[i] != fill)
            break;
    }

    return (words - i) * (unsigned int)sizeof(uint32_t);
}

int mc_fd_audio_pico_run_control(mc_fd_audio_pico_control_fn fn,
                                 void *user,
                                 unsigned int timeout_us)
{
    uint64_t deadline;
    uint32_t request;

    if (!fn)
        return 0;

    /*
     * Before the dedicated producer exists (or when already called on Core 1),
     * execute directly.  This preserves startup behavior and makes the bridge
     * usable on the same code path before transport launch.
     */
    if (!mc_fd_pico_core1_started ||
        !mc_fd_pico_core1_running ||
        get_core_num() == 1u)
    {
        fn(user);
        return 1;
    }

    if (timeout_us == 0u)
        timeout_us = 250000u;

    deadline = time_us_64() + (uint64_t)timeout_us;

    /*
     * Only one synchronous control transaction is allowed at a time.  Doom's
     * main thread is the normal caller, but this also makes an accidental
     * nested/overlapping request fail boundedly instead of corrupting payload.
     */
    while (mc_fd_pico_control_request_seq != mc_fd_pico_control_done_seq)
    {
        if (time_us_64() >= deadline)
        {
            ++mc_fd_pico_control_timeout_count;
            return 0;
        }
        tight_loop_contents();
    }

    request = mc_fd_pico_control_request_seq + 1u;
    if (request == 0u)
        request = 1u;

    mc_fd_pico_control_fn_ptr = fn;
    mc_fd_pico_control_user = user;
    __dmb();
    mc_fd_pico_control_request_seq = request;
    __dmb();

    while (mc_fd_pico_control_done_seq != request)
    {
        if (time_us_64() >= deadline)
        {
            /*
             * Do not revoke the callback after publication: Core 1 may already
             * be entering it.  The bridge uses persistent request storage, so a
             * late completion cannot dereference a dead stack object.
             */
            ++mc_fd_pico_control_timeout_count;
            return 0;
        }
        tight_loop_contents();
    }

    __dmb();
    mc_fd_pico_control_fn_ptr = NULL;
    mc_fd_pico_control_user = NULL;
    return 1;
}

unsigned long mc_fd_audio_pico_control_timeouts(void)
{
    return mc_fd_pico_control_timeout_count;
}

int mc_fd_audio_pico_control_paused(void)
{
    return mc_fd_pico_control_request_seq != mc_fd_pico_control_done_seq;
}
