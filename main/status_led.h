/*
 * status_led.h - on-board addressable RGB LED (WS2812 on GPIO27 of the
 * ESP32-C5-DevKitC-1) used as network + knob-mode indicator.
 *
 *   network: SoftAP / portal (local)  -> slow magenta blink
 *            STA connecting            -> fast yellow blink
 *            STA connected             -> steady knob-mode colour:
 *                                         VOLUME = cyan, STATION = orange
 *   override (e.g. long-press feedback) -> solid colour for a limited time
 *
 * A 250 ms esp_timer polls the Wi-Fi manager and renders; every API call is
 * task-context safe (mutex protected) and cheap.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

esp_err_t status_led_init(void);

/* mode 0 = volume, 1 = station (same encoding as app_status.encoder_mode). */
void status_led_set_knob_mode(uint8_t mode);

/* Show a solid colour for 'ms' milliseconds (0 = until status_led_clear_override()). */
void status_led_set_override(uint8_t r, uint8_t g, uint8_t b, uint32_t ms);
void status_led_clear_override(void);

void status_led_off(void);
