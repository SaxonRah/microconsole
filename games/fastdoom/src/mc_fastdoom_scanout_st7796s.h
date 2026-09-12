#ifndef MC_FASTDOOM_SCANOUT_ST7796S_H
#define MC_FASTDOOM_SCANOUT_ST7796S_H

#include <stdint.h>
#include "mr_pico_ili9341.h"

#ifdef __cplusplus
extern "C" {
#endif

int mc_fd_st7796s_scanout_start(
    mr_pico_ili9341_t *lcd,
    const volatile uint8_t *source_index8,
    const volatile uint16_t *palette565,
    const volatile int *palette_index);

void mc_fd_st7796s_scanout_publish(
    const volatile uint8_t *source_index8,
    int palette_index);

void mc_fd_st7796s_scanout_stop(void);

int mc_fd_st7796s_scanout_set_phases(unsigned int phases);
unsigned int mc_fd_st7796s_scanout_get_phases(void);
unsigned int mc_fd_st7796s_scanout_get_requested_phases(void);

int mc_fd_st7796s_scanout_running(void);
unsigned long mc_fd_st7796s_scanout_blocks(void);
unsigned long mc_fd_st7796s_scanout_phases(void);
unsigned long mc_fd_st7796s_scanout_cycles(void);
unsigned long mc_fd_st7796s_scanout_published_frames(void);
unsigned long mc_fd_st7796s_scanout_latched_frames(void);
unsigned long mc_fd_st7796s_scanout_replaced_pending_frames(void);
unsigned int mc_fd_st7796s_scanout_phase_hz10(void);
unsigned int mc_fd_st7796s_scanout_cycle_hz10(void);
uint32_t mc_fd_st7796s_scanout_last_service_us(void);
uint32_t mc_fd_st7796s_scanout_max_service_us(void);
uint32_t mc_fd_st7796s_scanout_last_publish_us(void);
uint32_t mc_fd_st7796s_scanout_max_publish_us(void);

#ifdef __cplusplus
}
#endif

#endif
