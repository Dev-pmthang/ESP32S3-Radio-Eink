/*
 * app_events.h - shared command and state definitions.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Commands accepted by the command manager (section 2.1 of the spec). */
typedef enum {
    CMD_PLAY_STATION = 0,   /* arg = station index, or -1 for current station */
    CMD_NEXT_STATION,
    CMD_PREVIOUS_STATION,
    CMD_STOP,
    CMD_RESUME,
    CMD_SET_VOLUME,         /* arg = 0..100 */
    CMD_WIFI_CONFIG,        /* ssid/password fields valid */
    CMD_SET_MONO,           /* arg = 1 mono downmix, 0 stereo */
    CMD_SET_DISPLAY,        /* arg = (type << 16) | rot_deg; 0xFFFF = unchanged */
    CMD_WIFI_RESET,         /* erase stored Wi-Fi credentials and restart */
    CMD_SAVE_SETTINGS,      /* internal: persist settings to NVS */
    CMD_TYPE_MAX
} app_cmd_type_t;

typedef enum {
    DISPLAY_EPD_154 = 0,    /* Waveshare 1.54" E-Ink (SSD1681, 200x200) */
    DISPLAY_ST7789_240 = 1, /* 1.54" TFT ST7789 (240x240, SPI) */
    DISPLAY_TYPE_MAX
} display_type_t;

static inline const char *display_type_str(display_type_t t)
{
    switch (t) {
    case DISPLAY_EPD_154:    return "epd_154";
    case DISPLAY_ST7789_240: return "st7789_240";
    default:                 return "epd_154";
    }
}

typedef struct {
    app_cmd_type_t type;
    int32_t arg;
    char ssid[33];
    char password[65];
} app_cmd_t;

/* Playback state shown on the dashboard. */
typedef enum {
    PB_STOPPED = 0,
    PB_CONNECTING,
    PB_BUFFERING,
    PB_PLAYING,
    PB_ERROR
} playback_state_t;

static inline const char *playback_state_str(playback_state_t s)
{
    switch (s) {
    case PB_STOPPED:    return "stopped";
    case PB_CONNECTING: return "connecting";
    case PB_BUFFERING:  return "buffering";
    case PB_PLAYING:    return "playing";
    case PB_ERROR:      return "error";
    default:            return "unknown";
    }
}
