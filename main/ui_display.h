/*
 * ui_display.h - Waveshare 1.54" e-Paper Module (200x200, SSD1681, SPI).
 *
 * Driver: SPI2 master + GPIO (CS, DC, RST, BUSY).
 * Rendering: 5000-byte static 1-bit monochrome framebuffer (200x200 / 8).
 * A low-priority task polls radio state every APP_EPD_REFRESH_MS and triggers
 * a partial (or periodic full) e-paper refresh only when content changes.
 */
#pragma once

#include "esp_err.h"

esp_err_t ui_display_init(void);
