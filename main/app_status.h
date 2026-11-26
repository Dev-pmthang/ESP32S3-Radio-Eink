/*
 * app_status.h - mutex protected snapshot of the radio state.
 *
 * Writers: command manager, stream task, decoder task, Wi-Fi manager.
 * Readers: web server (every 800 ms), rotary encoder task, monitor.
 * All setters are cheap (short critical section); none blocks for long.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "app_events.h"

typedef struct {
    playback_state_t playback;
    int              station_index;
    char             station_name[64];
    char             genre[32];
    char             stream_title[128];
    char             error[64];
    uint32_t         sample_rate;
    uint8_t          channels;
    uint16_t         bitrate_kbps;
    uint8_t          volume;
    bool             unmuted;
    uint8_t          level;          /* 0..100 signal level (peak based) */
    uint32_t         underruns;
    uint32_t         reconnects;
    uint32_t         decoder_errors;
    uint8_t          encoder_mode;   /* 0 = knob changes volume, 1 = knob changes station */
    bool             mono;           /* mono downmix active */
    display_type_t   display_type;   /* active display driver */
    uint16_t         display_rot;    /* 0, 90, 180, 270 */
    uint8_t          battery_pct;    /* 0..100 (simulated until ADC/fuel-gauge added) */
} app_status_t;

esp_err_t app_status_init(void);
void app_status_get(app_status_t *out);

void app_status_set_playback(playback_state_t st);
playback_state_t app_status_get_playback(void);
void app_status_set_station(int index, const char *name, const char *genre);
void app_status_set_stream_title(const char *title);
void app_status_set_error(const char *msg);
void app_status_set_audio_format(uint32_t rate, uint8_t channels, uint16_t kbps);
void app_status_set_volume(uint8_t vol);
void app_status_set_unmuted(bool unmuted);
void app_status_set_level(uint8_t level);
void app_status_inc_underruns(void);
void app_status_inc_reconnects(void);
void app_status_inc_decoder_errors(void);
void app_status_set_encoder_mode(uint8_t mode);
void app_status_set_mono(bool mono);
void app_status_set_display(display_type_t type, uint16_t rot);
void app_status_set_battery(uint8_t pct);
