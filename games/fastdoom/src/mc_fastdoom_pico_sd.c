/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MicroConsole FastDoom SD-card hardware configuration.
 *
 * ILI9341 display remains on SPI0:
 *   GP4  MISO
 *   GP5  LCD CS
 *   GP6  SCK
 *   GP7  MOSI
 *   GP8  RST
 *   GP9  DC
 *
 * microSD socket uses SPI1:
 *   GP12 MISO / DO
 *   GP13 CS
 *   GP14 SCK
 *   GP15 MOSI / DI
 *
 * MicroWave I2S therefore uses GP20 for DATA; BCLK/LRCLK remain GP10/GP11.
 */

#include <stddef.h>

#include "hardware/spi.h"
#include "hw_config.h"
#include "sd_card.h"

#ifndef MC_SD_MISO
#define MC_SD_MISO 12
#endif
#ifndef MC_SD_CS
#define MC_SD_CS 13
#endif
#ifndef MC_SD_SCK
#define MC_SD_SCK 14
#endif
#ifndef MC_SD_MOSI
#define MC_SD_MOSI 15
#endif
#ifndef MC_SD_BAUD
#define MC_SD_BAUD 12500000u
#endif

static spi_t g_mc_fd_sd_spi = {
    .hw_inst = spi1,
    .miso_gpio = MC_SD_MISO,
    .mosi_gpio = MC_SD_MOSI,
    .sck_gpio = MC_SD_SCK,
    .baud_rate = MC_SD_BAUD,
    .spi_mode = 0,
    .no_miso_gpio_pull_up = false,
    .set_drive_strength = false,
    .use_static_dma_channels = false,
};

static sd_spi_if_t g_mc_fd_sd_spi_if = {
    .spi = &g_mc_fd_sd_spi,
    .ss_gpio = MC_SD_CS,
    .set_drive_strength = false,
};

static sd_card_t g_mc_fd_sd_card = {
    .type = SD_IF_SPI,
    .spi_if_p = &g_mc_fd_sd_spi_if,
    .use_card_detect = false,
};

size_t sd_get_num(void)
{
    return 1u;
}

sd_card_t *sd_get_by_num(size_t num)
{
    return (num == 0u) ? &g_mc_fd_sd_card : NULL;
}
