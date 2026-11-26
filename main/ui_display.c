/*
 * ui_display.c - Dual-backend modern "Audio Card" UI sharing SPI2 pins:
 *   - DISPLAY_EPD_154    : Waveshare 1.54" e-Paper (200x200, SSD1681, 1-bit)
 *   - DISPLAY_ST7789_240 : 1.54" TFT ST7789 (240x240, RGB565, esp_lcd)
 *
 * Layout:
 *   Top bar      : Wi-Fi dot + IP/AP               | MODE | BAT % + icon
 *   Upper header : Rounded cover art (left) + genre/#idx + Station Name (right)
 *   Player card  : Rounded card with state, song title, circular mode badge,
 *                  Prev / Play-Pause / Next / Speaker, and Volume/Buffer bar.
 */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_attr.h"

#include "app_config.h"
#include "app_status.h"
#include "station_list.h"
#include "station_logos.h"
#include "stream_manager.h"
#include "wifi_manager.h"
#include "ui_display.h"

static const char *TAG = "disp";

/* ---- sin/cos LUT, Q10 (round(sin(deg) * 1024)) ---------------------------- */
static const int16_t sin_lut[360] = {
       0,   18,   36,   54,   71,   89,  107,  125,  143,  160,  178,  195,  213,  230,  248,  265,
     282,  299,  316,  333,  350,  367,  384,  400,  416,  433,  449,  465,  481,  496,  512,  527,
     543,  558,  573,  587,  602,  616,  630,  644,  658,  672,  685,  698,  711,  724,  737,  749,
     761,  773,  784,  796,  807,  818,  828,  839,  849,  859,  868,  878,  887,  896,  904,  912,
     920,  928,  935,  943,  949,  956,  962,  968,  974,  979,  984,  989,  994,  998, 1002, 1005,
    1008, 1011, 1014, 1016, 1018, 1020, 1022, 1023, 1023, 1024, 1024, 1024, 1023, 1023, 1022, 1020,
    1018, 1016, 1014, 1011, 1008, 1005, 1002,  998,  994,  989,  984,  979,  974,  968,  962,  956,
     949,  943,  935,  928,  920,  912,  904,  896,  887,  878,  868,  859,  849,  839,  828,  818,
     807,  796,  784,  773,  761,  749,  737,  724,  711,  698,  685,  672,  658,  644,  630,  616,
     602,  587,  573,  558,  543,  527,  512,  496,  481,  465,  449,  433,  416,  400,  384,  367,
     350,  333,  316,  299,  282,  265,  248,  230,  213,  195,  178,  160,  143,  125,  107,   89,
      71,   54,   36,   18,    0,  -18,  -36,  -54,  -71,  -89, -107, -125, -143, -160, -178, -195,
    -213, -230, -248, -265, -282, -299, -316, -333, -350, -367, -384, -400, -416, -433, -449, -465,
    -481, -496, -512, -527, -543, -558, -573, -587, -602, -616, -630, -644, -658, -672, -685, -698,
    -711, -724, -737, -749, -761, -773, -784, -796, -807, -818, -828, -839, -849, -859, -868, -878,
    -887, -896, -904, -912, -920, -928, -935, -943, -949, -956, -962, -968, -974, -979, -984, -989,
    -994, -998,-1002,-1005,-1008,-1011,-1014,-1016,-1018,-1020,-1022,-1023,-1023,-1024,-1024,-1024,
   -1023,-1023,-1022,-1020,-1018,-1016,-1014,-1011,-1008,-1005,-1002, -998, -994, -989, -984, -979,
    -974, -968, -962, -956, -949, -943, -935, -928, -920, -912, -904, -896, -887, -878, -868, -859,
    -849, -839, -828, -818, -807, -796, -784, -773, -761, -749, -737, -724, -711, -698, -685, -672,
    -658, -644, -630, -616, -602, -587, -573, -558, -543, -527, -512, -496, -481, -465, -449, -433,
    -416, -400, -384, -367, -350, -333, -316, -299, -282, -265, -248, -230, -213, -195, -178, -160,
    -143, -125, -107,  -89,  -71,  -54,  -36,  -18
};

static inline int32_t cos_q10(int deg)
{
    return sin_lut[(deg + 90) % 360];
}

/* ---- 5x7 font, ASCII 0x20..0x7E (column major) --------------------------- */
static const uint8_t kFont5x7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00}, {0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7F,0x14,0x7F,0x14}, {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00}, {0x00,0x1C,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1C,0x00}, {0x14,0x08,0x3E,0x08,0x14}, {0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08}, {0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02}, {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31}, {0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39}, {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}, {0x00,0x36,0x36,0x00,0x00},
    {0x00,0x56,0x36,0x00,0x00}, {0x08,0x14,0x22,0x41,0x00}, {0x14,0x14,0x14,0x14,0x14},
    {0x00,0x41,0x22,0x14,0x08}, {0x02,0x01,0x51,0x09,0x06}, {0x32,0x49,0x79,0x41,0x3E},
    {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F}, {0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43}, {0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20}, {0x00,0x41,0x41,0x7F,0x00}, {0x04,0x02,0x01,0x02,0x04},
    {0x40,0x40,0x40,0x40,0x40}, {0x00,0x01,0x02,0x04,0x00}, {0x20,0x54,0x54,0x54,0x78},
    {0x7F,0x48,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x20}, {0x38,0x44,0x44,0x48,0x7F},
    {0x38,0x54,0x54,0x54,0x18}, {0x08,0x7E,0x09,0x01,0x02}, {0x0C,0x52,0x52,0x52,0x3E},
    {0x7F,0x08,0x04,0x04,0x78}, {0x00,0x44,0x7D,0x40,0x00}, {0x20,0x40,0x44,0x3D,0x00},
    {0x7F,0x10,0x28,0x44,0x00}, {0x00,0x41,0x7F,0x40,0x00}, {0x7C,0x04,0x18,0x04,0x78},
    {0x7C,0x08,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38}, {0x7C,0x14,0x14,0x14,0x08},
    {0x08,0x14,0x14,0x18,0x7C}, {0x7C,0x08,0x04,0x04,0x08}, {0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20}, {0x3C,0x40,0x40,0x20,0x7C}, {0x1C,0x20,0x40,0x20,0x1C},
    {0x3C,0x40,0x30,0x40,0x3C}, {0x44,0x28,0x10,0x28,0x44}, {0x0C,0x50,0x50,0x50,0x3C},
    {0x44,0x64,0x54,0x4C,0x44}, {0x00,0x08,0x36,0x41,0x00}, {0x00,0x00,0x7F,0x00,0x00},
    {0x00,0x41,0x36,0x08,0x00}, {0x08,0x04,0x08,0x10,0x08}
};

/* ---- shared UI model ---------------------------------------------------- */
typedef struct {
    char     ip[16];
    bool     wifi_connected;
    bool     ap_active;
    uint8_t  knob_mode;
    bool     mono;
    char     station[64];
    char     genre[32];
    char     title[128];
    playback_state_t playback;
    uint32_t rate;
    uint16_t kbps;
    uint8_t  volume;
    uint8_t  buffer_pct;
    int      station_index;
    int      rssi;
    uint16_t rotation;
    uint8_t  battery_pct;
    int      disc_deg;
} ui_model_t;

static TaskHandle_t s_task = NULL;

static int text_width(const char *s, int scale)
{
    size_t n = strlen(s);
    return n == 0 ? 0 : (int)(n * 6 * scale - scale);
}

static void scopy(char *dst, size_t dst_sz, const char *src)
{
    size_t n = strlen(src);
    if (n >= dst_sz) n = dst_sz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void split_title(const ui_model_t *m, char *song, size_t song_sz, char *artist, size_t art_sz)
{
    const char *raw = m->title;
    artist[0] = '\0';
    if (raw[0] == '\0') {
        scopy(song, song_sz, m->playback == PB_PLAYING ? "Live broadcast" :
              (m->playback == PB_STOPPED ? "Paused / Stopped" : "Connecting stream..."));
        return;
    }
    const char *sep = strstr(raw, " - ");
    if (sep != NULL && sep > raw && sep[3] != '\0') {
        size_t alen = (size_t)(sep - raw);
        if (alen >= art_sz) alen = art_sz - 1;
        memcpy(artist, raw, alen);
        artist[alen] = '\0';
        scopy(song, song_sz, sep + 3);
    } else {
        scopy(song, song_sz, raw);
    }
}

static void model_capture(ui_model_t *m)
{
    app_status_t st;
    app_status_get(&st);
    wifi_manager_get_ip(m->ip, sizeof(m->ip));
    m->wifi_connected = wifi_manager_is_connected();
    m->ap_active = wifi_manager_is_ap_active();
    m->knob_mode = st.encoder_mode;
    m->mono = st.mono;
    memcpy(m->station, st.station_name, sizeof(m->station));
    memcpy(m->genre, st.genre, sizeof(m->genre));
    memcpy(m->title, st.stream_title, sizeof(m->title));
    m->playback = st.playback;
    m->rate = st.sample_rate;
    m->kbps = st.bitrate_kbps;
    m->volume = st.volume;
    size_t cap = stream_manager_capacity();
    m->buffer_pct = cap ? (uint8_t)(stream_manager_bytes_available() * 100 / cap) : 0;
    m->station_index = st.station_index;
    m->rssi = wifi_manager_get_rssi();
    m->rotation = st.display_rot;
    m->battery_pct = st.battery_pct;
}

/* Word-wrap helper: fills up to max_lines lines of at most max_chars characters. */
static int wrap_lines(const char *src, int max_chars, int max_lines, char lines[][48])
{
    int count = 0;
    const char *p = src;
    while (*p && count < max_lines) {
        while (*p == ' ') p++;
        if (*p == '\0') break;
        int len = (int)strlen(p);
        if (len <= max_chars) {
            scopy(lines[count], 48, p);
            count++;
            break;
        }
        int cut = max_chars;
        if (count < max_lines - 1) {
            int sp = cut;
            while (sp > 0 && p[sp] != ' ') sp--;
            if (sp > max_chars / 3) cut = sp;
        }
        if (cut >= 47) cut = 47;
        memcpy(lines[count], p, (size_t)cut);
        lines[count][cut] = '\0';
        count++;
        p += cut;
    }
    return count;
}

/* =========================================================================
 * BACKEND 1: Waveshare 1.54" e-Paper (200x200, SSD1681)
 * ========================================================================= */
#define EPD_W           200
#define EPD_H           200
#define EPD_STRIDE      (EPD_W / 8)
#define EPD_BUF_SIZE    (EPD_STRIDE * EPD_H)
#define EPD_WHITE       1
#define EPD_BLACK       0
#define EPD_SPIN_DEG    45
#define EPD_SPIN_MS     900

static uint8_t             s_fb[EPD_BUF_SIZE];
static spi_device_handle_t s_spi = NULL;
static uint16_t            s_epd_rot = 0;

static void epd_write_cmd(uint8_t cmd)
{
    spi_transaction_t t = { .length = 8, .tx_buffer = &cmd };
    gpio_set_level(APP_EPD_DC_GPIO, 0);
    spi_device_polling_transmit(s_spi, &t);
}

static void epd_write_data(const uint8_t *data, size_t len)
{
    if (len == 0) return;
    spi_transaction_t t = { .length = len * 8, .tx_buffer = data };
    gpio_set_level(APP_EPD_DC_GPIO, 1);
    spi_device_polling_transmit(s_spi, &t);
}

static void epd_write_byte(uint8_t b)
{
    epd_write_data(&b, 1);
}

static void epd_wait_busy(uint32_t timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (gpio_get_level(APP_EPD_BUSY_GPIO) == 1) {
        if (esp_timer_get_time() > deadline) {
            ESP_LOGW(TAG, "EPD BUSY timeout (%lu ms)", (unsigned long)timeout_ms);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void epd_hw_reset(void)
{
    gpio_set_level(APP_EPD_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(APP_EPD_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(APP_EPD_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    epd_wait_busy(2000);
}

static void epd_set_window_and_cursor(void)
{
    const uint8_t x_win[2] = { 0x00, EPD_STRIDE - 1 };
    const uint8_t y_win[4] = { 0x00, 0x00, (EPD_H - 1) & 0xFF, ((EPD_H - 1) >> 8) & 0xFF };
    const uint8_t y_cur[2] = { 0x00, 0x00 };

    epd_write_cmd(0x44);
    epd_write_data(x_win, sizeof(x_win));
    epd_write_cmd(0x45);
    epd_write_data(y_win, sizeof(y_win));
    epd_write_cmd(0x4E);
    epd_write_byte(0x00);
    epd_write_cmd(0x4F);
    epd_write_data(y_cur, sizeof(y_cur));
}

static void epd_init_panel(void)
{
    const uint8_t drv_out[3] = { (EPD_H - 1) & 0xFF, ((EPD_H - 1) >> 8) & 0xFF, 0x00 };
    epd_hw_reset();
    epd_write_cmd(0x12);
    epd_wait_busy(2000);
    epd_write_cmd(0x01);
    epd_write_data(drv_out, sizeof(drv_out));
    epd_write_cmd(0x11);
    epd_write_byte(0x03);
    epd_set_window_and_cursor();
    epd_write_cmd(0x3C);
    epd_write_byte(0x05);
    epd_write_cmd(0x18);
    epd_write_byte(0x80);
    epd_wait_busy(2000);
}

static void epd_display_full(void)
{
    epd_init_panel();
    epd_write_cmd(0x24);
    epd_write_data(s_fb, EPD_BUF_SIZE);
    epd_set_window_and_cursor();
    epd_write_cmd(0x26);
    epd_write_data(s_fb, EPD_BUF_SIZE);
    epd_write_cmd(0x22);
    epd_write_byte(0xF7);
    epd_write_cmd(0x20);
    epd_wait_busy(5000);
}

static void epd_display_partial(void)
{
    epd_hw_reset();
    epd_write_cmd(0x3C);
    epd_write_byte(0x80);
    epd_write_cmd(0x11);
    epd_write_byte(0x03);
    epd_set_window_and_cursor();
    epd_write_cmd(0x24);
    epd_write_data(s_fb, EPD_BUF_SIZE);
    epd_write_cmd(0x22);
    epd_write_byte(0xFF);
    epd_write_cmd(0x20);
    epd_wait_busy(3000);
}

static inline void epd_put_pixel(int x, int y, uint8_t col)
{
    if (x < 0 || x >= EPD_W || y < 0 || y >= EPD_H) return;
    int px, py;
    switch (s_epd_rot) {
    case 90:  px = EPD_W - 1 - y; py = x; break;
    case 180: px = EPD_W - 1 - x; py = EPD_H - 1 - y; break;
    case 270: px = y;             py = EPD_H - 1 - x; break;
    default:  px = x;             py = y; break;
    }
    size_t idx = (size_t)py * EPD_STRIDE + (size_t)(px >> 3);
    uint8_t mask = (uint8_t)(0x80U >> (px & 7));
    if (col) s_fb[idx] |= mask;
    else     s_fb[idx] &= (uint8_t)~mask;
}

static void epd_fill_rect(int x, int y, int w, int h, uint8_t col)
{
    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) epd_put_pixel(x + dx, y + dy, col);
    }
}

static void epd_draw_hline(int x, int y, int w, uint8_t col)
{
    for (int dx = 0; dx < w; dx++) epd_put_pixel(x + dx, y, col);
}

static void epd_draw_rect(int x, int y, int w, int h, uint8_t col)
{
    epd_draw_hline(x, y, w, col);
    epd_draw_hline(x, y + h - 1, w, col);
    for (int dy = 0; dy < h; dy++) {
        epd_put_pixel(x, y + dy, col);
        epd_put_pixel(x + w - 1, y + dy, col);
    }
}

static void epd_fill_circle(int cx, int cy, int r, uint8_t col)
{
    int r2 = r * r;
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy <= r2) epd_put_pixel(cx + dx, cy + dy, col);
        }
    }
}

static void epd_draw_circle(int cx, int cy, int r, uint8_t col)
{
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        epd_put_pixel(cx + x, cy + y, col); epd_put_pixel(cx + y, cy + x, col);
        epd_put_pixel(cx - y, cy + x, col); epd_put_pixel(cx - x, cy + y, col);
        epd_put_pixel(cx - x, cy - y, col); epd_put_pixel(cx - y, cy - x, col);
        epd_put_pixel(cx + y, cy - x, col); epd_put_pixel(cx + x, cy - y, col);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x + 1); }
    }
}

static void epd_fill_round_rect(int x, int y, int w, int h, int r, uint8_t col)
{
    epd_fill_rect(x + r, y, w - 2 * r, h, col);
    epd_fill_rect(x, y + r, r, h - 2 * r, col);
    epd_fill_rect(x + w - r, y + r, r, h - 2 * r, col);
    epd_fill_circle(x + r, y + r, r, col);
    epd_fill_circle(x + w - r - 1, y + r, r, col);
    epd_fill_circle(x + r, y + h - r - 1, r, col);
    epd_fill_circle(x + w - r - 1, y + h - r - 1, r, col);
}

static void epd_draw_round_rect(int x, int y, int w, int h, int r, uint8_t col)
{
    epd_draw_hline(x + r, y, w - 2 * r, col);
    epd_draw_hline(x + r, y + h - 1, w - 2 * r, col);
    for (int dy = r; dy < h - r; dy++) {
        epd_put_pixel(x, y + dy, col);
        epd_put_pixel(x + w - 1, y + dy, col);
    }
    int px = r, py = 0, err = 1 - r;
    int cx0 = x + r, cx1 = x + w - r - 1;
    int cy0 = y + r, cy1 = y + h - r - 1;
    while (px >= py) {
        epd_put_pixel(cx0 - px, cy0 - py, col); epd_put_pixel(cx0 - py, cy0 - px, col);
        epd_put_pixel(cx1 + px, cy0 - py, col); epd_put_pixel(cx1 + py, cy0 - px, col);
        epd_put_pixel(cx0 - px, cy1 + py, col); epd_put_pixel(cx0 - py, cy1 + px, col);
        epd_put_pixel(cx1 + px, cy1 + py, col); epd_put_pixel(cx1 + py, cy1 + px, col);
        py++;
        if (err < 0) err += 2 * py + 1;
        else { px--; err += 2 * (py - px + 1); }
    }
}

static void epd_draw_line(int x0, int y0, int x1, int y1, uint8_t col)
{
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int steps = adx > ady ? adx : ady;
    if (steps == 0) { epd_put_pixel(x0, y0, col); return; }
    for (int i = 0; i <= steps; i++) {
        epd_put_pixel(x0 + dx * i / steps, y0 + dy * i / steps, col);
    }
}

static void epd_draw_wide_line(int x0, int y0, int x1, int y1, int r, uint8_t col)
{
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int steps = adx > ady ? adx : ady;
    if (steps == 0) steps = 1;
    for (int i = 0; i <= steps; i++) {
        epd_fill_circle(x0 + dx * i / steps, y0 + dy * i / steps, r, col);
    }
}

static void epd_draw_text(int x, int y, const char *s, int scale, uint8_t col)
{
    for (size_t i = 0; s[i] != '\0'; i++) {
        int cx = x + (int)i * 6 * scale;
        if (cx >= EPD_W) break;
        if (cx + 5 * scale < 0) continue;
        uint8_t ch = (uint8_t)s[i];
        if (ch < 0x20 || ch > 0x7E) ch = '?';
        const uint8_t *glyph = kFont5x7[ch - 0x20];
        for (int gx = 0; gx < 5; gx++) {
            uint8_t bits = glyph[gx];
            for (int gy = 0; gy < 7; gy++) {
                if (bits & (1U << gy)) {
                    epd_fill_rect(cx + gx * scale, y + gy * scale, scale, scale, col);
                }
            }
        }
    }
}

static void epd_draw_battery(int x, int y, uint8_t pct)
{
    epd_draw_rect(x, y, 15, 8, EPD_BLACK);
    epd_fill_rect(x + 15, y + 2, 2, 4, EPD_BLACK);
    int fill_w = (11 * pct + 50) / 100;
    if (fill_w > 11) fill_w = 11;
    if (fill_w > 0) epd_fill_rect(x + 2, y + 2, fill_w, 4, EPD_BLACK);
}

static void epd_draw_speaker(int x, int y, uint8_t vol)
{
    /* Small speaker cone */
    epd_fill_rect(x, y + 3, 3, 5, EPD_BLACK);
    epd_draw_line(x + 3, y + 3, x + 6, y, EPD_BLACK);
    epd_draw_line(x + 3, y + 7, x + 6, y + 10, EPD_BLACK);
    epd_fill_rect(x + 6, y, 1, 11, EPD_BLACK);
    if (vol == 0) {
        epd_draw_line(x + 8, y + 3, x + 12, y + 7, EPD_BLACK);
        epd_draw_line(x + 8, y + 7, x + 12, y + 3, EPD_BLACK);
    } else {
        epd_fill_rect(x + 8, y + 3, 1, 5, EPD_BLACK);
        if (vol > 50) epd_fill_rect(x + 10, y + 2, 1, 7, EPD_BLACK);
    }
}

static void epd_render_frame(const ui_model_t *m)
{
    s_epd_rot = m->rotation;
    memset(s_fb, 0xFF, sizeof(s_fb));
    const bool playing = (m->playback == PB_PLAYING);

    /* 1. Top status bar (y = 0..14) */
    if (m->wifi_connected) {
        epd_fill_circle(6, 7, 3, EPD_BLACK);
        epd_draw_text(12, 4, m->ip, 1, EPD_BLACK);
    } else if (m->ap_active) {
        epd_draw_circle(6, 7, 3, EPD_BLACK);
        epd_draw_text(12, 4, "AP 192.168.4.1", 1, EPD_BLACK);
    } else {
        epd_draw_circle(6, 7, 3, EPD_BLACK);
        epd_draw_text(12, 4, "connecting...", 1, EPD_BLACK);
    }

    char bat_txt[8];
    snprintf(bat_txt, sizeof(bat_txt), "%u%%", (unsigned)m->battery_pct);
    int bw = text_width(bat_txt, 1);
    epd_draw_battery(EPD_W - 21, 3, m->battery_pct);
    epd_draw_text(EPD_W - 24 - bw, 4, bat_txt, 1, EPD_BLACK);
    epd_draw_hline(6, 14, EPD_W - 12, EPD_BLACK);

    /* 2. Upper Header: Rounded Station Logo (left) + Metadata & Station Name (right) */
    const int art_x = 8, art_y = 20, art_s = 56;
    int logo_idx = m->station_index;
    if (logo_idx < 0 || logo_idx >= (int)(sizeof(kStationLogos) / sizeof(kStationLogos[0]))) {
        logo_idx = 0;
    }
    const uint8_t *logo = kStationLogos[logo_idx];
    for (int ly = 0; ly < STATION_LOGO_H; ly++) {
        for (int lx = 0; lx < STATION_LOGO_W; lx++) {
            /* Clip 4x4 corner pixels for rounded-square look */
            bool corner = (lx < 4 && ly < 4 && (3 - lx) * (3 - lx) + (3 - ly) * (3 - ly) > 12) ||
                          (lx >= art_s - 4 && ly < 4 && (lx - (art_s - 4)) * (lx - (art_s - 4)) + (3 - ly) * (3 - ly) > 12) ||
                          (lx < 4 && ly >= art_s - 4 && (3 - lx) * (3 - lx) + (ly - (art_s - 4)) * (ly - (art_s - 4)) > 12) ||
                          (lx >= art_s - 4 && ly >= art_s - 4 && (lx - (art_s - 4)) * (lx - (art_s - 4)) + (ly - (art_s - 4)) * (ly - (art_s - 4)) > 12);
            if (corner) continue;
            uint8_t bit = (logo[ly * STATION_LOGO_STRIDE + (lx >> 3)] >> (7 - (lx & 7))) & 1U;
            epd_put_pixel(art_x + lx, art_y + ly, bit ? EPD_WHITE : EPD_BLACK);
        }
    }
    epd_draw_round_rect(art_x, art_y, art_s, art_s, 6, EPD_BLACK);

    /* Right of cover art (x = 72..194) */
    char meta[64];
    snprintf(meta, sizeof(meta), "%s . #%d/%u",
             m->genre[0] ? m->genre : "Radio",
             m->station_index + 1, (unsigned)station_list_count());
    if (text_width(meta, 1) > 122) meta[122 / 6] = '\0';
    epd_draw_text(72, 22, meta, 1, EPD_BLACK);

    char st_lines[2][48];
    int st_scale = (text_width(m->station, 2) <= 120) ? 2 : 1;
    int st_chars = (st_scale == 2) ? 10 : 20;
    int n_st = wrap_lines(m->station[0] ? m->station : "ESP32-S3 Radio", st_chars, 2, st_lines);
    for (int i = 0; i < n_st; i++) {
        int ty = 35 + i * (st_scale == 2 ? 17 : 10);
        epd_draw_text(72, ty, st_lines[i], st_scale, EPD_BLACK);
        if (st_scale == 1) epd_draw_text(73, ty, st_lines[i], st_scale, EPD_BLACK);
    }

    /* 3. Lower Rounded Player Card (x = 6, y = 84, w = 188, h = 110) */
    epd_draw_round_rect(6, 84, 188, 110, 8, EPD_BLACK);

    const char *st_str;
    switch (m->playback) {
    case PB_PLAYING:    st_str = "Playing";    break;
    case PB_BUFFERING:  st_str = "Buffering";  break;
    case PB_CONNECTING: st_str = "Connecting"; break;
    case PB_ERROR:      st_str = "Error";      break;
    default:            st_str = "Stopped";    break;
    }
    char sub_line[40];
    if (m->rate > 0 && m->kbps > 0) {
        snprintf(sub_line, sizeof(sub_line), "%s . %luHz . %uk",
                 st_str, (unsigned long)m->rate, (unsigned)m->kbps);
    } else {
        snprintf(sub_line, sizeof(sub_line), "%s . %s", st_str, m->mono ? "Mono" : "Stereo");
    }
    if (text_width(sub_line, 1) > 172) sub_line[172 / 6] = '\0';
    epd_draw_text(14, 92, sub_line, 1, EPD_BLACK);

    char song[96], artist[48], song_lines[2][48];
    split_title(m, song, sizeof(song), artist, sizeof(artist));
    int n_song = wrap_lines(song, 28, 2, song_lines);
    for (int i = 0; i < n_song; i++) {
        epd_draw_text(14, 104 + i * 10, song_lines[i], 1, EPD_BLACK);
        epd_draw_text(15, 104 + i * 10, song_lines[i], 1, EPD_BLACK);
    }
    if (n_song == 1 && artist[0] != '\0') {
        if (text_width(artist, 1) > 172) artist[172 / 6] = '\0';
        epd_draw_text(14, 115, artist, 1, EPD_BLACK);
    }

    /* Transport row (y = 148) */
    const int cy = 148;
    /* Circular knob-mode badge (like "1x" in reference) */
    epd_draw_circle(28, cy, 11, EPD_BLACK);
    const char *km = (m->knob_mode == 0) ? "VOL" : "STA";
    epd_draw_text(28 - text_width(km, 1) / 2, cy - 3, km, 1, EPD_BLACK);

    /* Previous |< */
    epd_fill_rect(60, cy - 4, 2, 9, EPD_BLACK);
    epd_draw_line(63, cy, 68, cy - 4, EPD_BLACK);
    epd_draw_line(63, cy, 68, cy + 4, EPD_BLACK);
    epd_draw_line(68, cy - 4, 68, cy + 4, EPD_BLACK);

    /* Play / Pause solid circle */
    epd_fill_circle(100, cy, 14, EPD_BLACK);
    if (playing) {
        epd_fill_rect(95, cy - 5, 3, 11, EPD_WHITE);
        epd_fill_rect(102, cy - 5, 3, 11, EPD_WHITE);
    } else {
        for (int dx = 0; dx < 8; dx++) {
            int half = (7 - dx) / 2;
            epd_fill_rect(97 + dx, cy - half, 1, half * 2 + 1, EPD_WHITE);
        }
    }

    /* Next >| */
    epd_draw_line(132, cy - 4, 132, cy + 4, EPD_BLACK);
    epd_draw_line(132, cy - 4, 137, cy, EPD_BLACK);
    epd_draw_line(132, cy + 4, 137, cy, EPD_BLACK);
    epd_fill_rect(138, cy - 4, 2, 9, EPD_BLACK);

    /* Speaker icon */
    epd_draw_speaker(166, cy - 5, m->volume);

    /* Bottom progress / volume bar inside card (y = 176) */
    char left_v[12], right_v[12];
    if (m->playback == PB_BUFFERING || m->playback == PB_CONNECTING) {
        snprintf(left_v, sizeof(left_v), "BUF%u%%", (unsigned)m->buffer_pct);
    } else {
        snprintf(left_v, sizeof(left_v), "%u%%", (unsigned)m->volume);
    }
    if (m->wifi_connected) snprintf(right_v, sizeof(right_v), "%ddB", m->rssi);
    else                   snprintf(right_v, sizeof(right_v), "--dB");

    epd_draw_text(14, 174, left_v, 1, EPD_BLACK);
    int rw = text_width(right_v, 1);
    epd_draw_text(186 - rw, 174, right_v, 1, EPD_BLACK);

    const int bar_x0 = 44, bar_x1 = 180 - rw, bar_w = bar_x1 - bar_x0;
    if (bar_w > 20) {
        epd_draw_hline(bar_x0, 177, bar_w, EPD_BLACK);
        int fill_w = bar_w * m->volume / 100;
        if (fill_w > 0) epd_fill_rect(bar_x0, 176, fill_w, 3, EPD_BLACK);
    }
}

static bool epd_model_changed(const ui_model_t *a, const ui_model_t *b)
{
    return strcmp(a->ip, b->ip) != 0 ||
           a->wifi_connected != b->wifi_connected ||
           a->ap_active != b->ap_active ||
           a->knob_mode != b->knob_mode ||
           a->mono != b->mono ||
           strcmp(a->station, b->station) != 0 ||
           strcmp(a->title, b->title) != 0 ||
           a->playback != b->playback ||
           a->volume != b->volume ||
           a->station_index != b->station_index ||
           (a->rssi / 10) != (b->rssi / 10) ||
           a->rotation != b->rotation ||
           a->battery_pct != b->battery_pct;
}

static void epd_ui_task(void *arg)
{
    (void)arg;
    static ui_model_t cur, last;
    memset(&cur, 0, sizeof(cur));
    model_capture(&cur);
    epd_render_frame(&cur);
    epd_display_full();
    last = cur;
    ESP_LOGI(TAG, "initial full E-Ink frame drawn (rot %u)", (unsigned)cur.rotation);

    uint32_t partial_cnt = 0;
    int64_t last_update_us = esp_timer_get_time();

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(APP_EPD_REFRESH_MS));
        int prev_deg = cur.disc_deg;
        model_capture(&cur);
        cur.disc_deg = prev_deg;

        bool changed = epd_model_changed(&cur, &last);
        int64_t now_us = esp_timer_get_time();

        if (!changed) {
            continue;
        }
        if ((now_us - last_update_us) < (int64_t)APP_EPD_MIN_UPDATE_MS * 1000 &&
            cur.rotation == last.rotation) {
            continue;
        }

        epd_render_frame(&cur);
        partial_cnt++;
        if (partial_cnt >= APP_EPD_FULL_EVERY || cur.rotation != last.rotation) {
            epd_display_full();
            partial_cnt = 0;
        } else {
            epd_display_partial();
        }
        last = cur;
        last_update_us = esp_timer_get_time();
    }
}

static esp_err_t epd_backend_init(void)
{
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << APP_EPD_DC_GPIO) | (1ULL << APP_EPD_RST_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&out_cfg), TAG, "DC/RST gpio_config");
    gpio_set_level(APP_EPD_DC_GPIO, 0);
    gpio_set_level(APP_EPD_RST_GPIO, 1);

    gpio_config_t busy_cfg = {
        .pin_bit_mask = (1ULL << APP_EPD_BUSY_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&busy_cfg), TAG, "BUSY gpio_config");

    spi_bus_config_t bus = {
        .mosi_io_num = APP_EPD_MOSI_GPIO,
        .miso_io_num = -1,
        .sclk_io_num = APP_EPD_CLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EPD_BUF_SIZE,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi_bus_initialize");

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = APP_EPD_SPI_HZ,
        .mode = 0,
        .spics_io_num = APP_EPD_CS_GPIO,
        .queue_size = 7,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(SPI2_HOST, &dev_cfg, &s_spi), TAG, "spi_bus_add_device");

    if (xTaskCreate(epd_ui_task, "epd_ui", APP_TASK_STACK_EPD, NULL, APP_TASK_PRIO_EPD, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Waveshare 1.54\" E-Ink ready: MOSI=%d CLK=%d CS=%d DC=%d RST=%d BUSY=%d",
             APP_EPD_MOSI_GPIO, APP_EPD_CLK_GPIO, APP_EPD_CS_GPIO,
             APP_EPD_DC_GPIO, APP_EPD_RST_GPIO, APP_EPD_BUSY_GPIO);
    return ESP_OK;
}

/* =========================================================================
 * BACKEND 2: 1.54" ST7789 240x240 TFT (sharing the same SPI2 pins)
 * ========================================================================= */
#define TFT_W           240
#define TFT_H           240
#define SURF_PIXELS     1920
#define RAW565(r, g, b) ((uint16_t)((((r) & 0xF8U) << 8) | (((g) & 0xFCU) << 3) | (((b) & 0xF8U) >> 3)))
#define BE565(r, g, b)  ((uint16_t)((RAW565(r, g, b) >> 8) | (RAW565(r, g, b) << 8)))

#define COL_BG          BE565(24, 20, 18)
#define COL_CARD        BE565(36, 30, 26)
#define COL_CARD_BORDER BE565(64, 54, 48)
#define COL_ACCENT      BE565(218, 128, 72)
#define COL_TEXT        BE565(244, 240, 234)
#define COL_MUTED       BE565(168, 158, 148)
#define COL_TRACK       BE565(82, 74, 68)
#define COL_OK          BE565(72, 196, 108)
#define COL_WARN        BE565(230, 170, 50)
#define COL_ERR         BE565(220, 75, 75)
#define COL_WHITE       0xFFFFU
#define COL_BLACK       0x0000U

#define FRAME_MS_PLAY   80
#define FRAME_MS_IDLE   200
#define DISC_STEP_DEG   12

static DMA_ATTR uint16_t s_buf[SURF_PIXELS];
static int s_sx0, s_sy0, s_sw, s_sh;
static esp_lcd_panel_io_handle_t s_io = NULL;
static esp_lcd_panel_handle_t    s_panel = NULL;
static SemaphoreHandle_t         s_dma_done = NULL;
static StaticSemaphore_t         s_dma_done_buf;

static bool IRAM_ATTR on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *ev, void *ctx)
{
    (void)io; (void)ev; (void)ctx;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_dma_done, &woken);
    return woken == pdTRUE;
}

static void surface_begin(int x0, int y0, int w, int h, uint16_t bg)
{
    s_sx0 = x0; s_sy0 = y0; s_sw = w; s_sh = h;
    int n = w * h;
    for (int i = 0; i < n; i++) s_buf[i] = bg;
}

static void surface_flush(void)
{
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, s_sx0, s_sy0, s_sx0 + s_sw, s_sy0 + s_sh, s_buf);
    if (err == ESP_OK) {
        xSemaphoreTake(s_dma_done, pdMS_TO_TICKS(100));
    }
}

static void render_region(int x0, int y0, int w, int h, uint16_t bg, void (*paint)(const ui_model_t *),
                          const ui_model_t *m)
{
    int max_rows = SURF_PIXELS / w;
    for (int y = y0; y < y0 + h; y += max_rows) {
        int rows = (y0 + h - y) < max_rows ? (y0 + h - y) : max_rows;
        surface_begin(x0, y, w, rows, bg);
        paint(m);
        surface_flush();
    }
}

static inline void tft_put_pixel(int x, int y, uint16_t col)
{
    int lx = x - s_sx0, ly = y - s_sy0;
    if (lx >= 0 && lx < s_sw && ly >= 0 && ly < s_sh) s_buf[ly * s_sw + lx] = col;
}

static void tft_fill_rect(int x, int y, int w, int h, uint16_t col)
{
    int lx0 = x - s_sx0, ly0 = y - s_sy0;
    int lx1 = lx0 + w, ly1 = ly0 + h;
    if (lx0 < 0) lx0 = 0;
    if (ly0 < 0) ly0 = 0;
    if (lx1 > s_sw) lx1 = s_sw;
    if (ly1 > s_sh) ly1 = s_sh;
    for (int r = ly0; r < ly1; r++) {
        uint16_t *row = &s_buf[r * s_sw];
        for (int c = lx0; c < lx1; c++) row[c] = col;
    }
}

static void tft_fill_circle(int cx, int cy, int r, uint16_t col)
{
    if (cy + r < s_sy0 || cy - r >= s_sy0 + s_sh) return;
    int r2 = r * r;
    for (int dy = -r; dy <= r; dy++) {
        int y = cy + dy;
        if (y < s_sy0 || y >= s_sy0 + s_sh) continue;
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy <= r2) tft_put_pixel(cx + dx, y, col);
        }
    }
}

static void tft_draw_circle(int cx, int cy, int r, uint16_t col)
{
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        tft_put_pixel(cx + x, cy + y, col); tft_put_pixel(cx + y, cy + x, col);
        tft_put_pixel(cx - y, cy + x, col); tft_put_pixel(cx - x, cy + y, col);
        tft_put_pixel(cx - x, cy - y, col); tft_put_pixel(cx - y, cy - x, col);
        tft_put_pixel(cx + y, cy - x, col); tft_put_pixel(cx + x, cy - y, col);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x + 1); }
    }
}

static void tft_draw_line(int x0, int y0, int x1, int y1, uint16_t col)
{
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int steps = adx > ady ? adx : ady;
    if (steps == 0) { tft_put_pixel(x0, y0, col); return; }
    for (int i = 0; i <= steps; i++) tft_put_pixel(x0 + dx * i / steps, y0 + dy * i / steps, col);
}

static void tft_draw_wide_line(int x0, int y0, int x1, int y1, int r, uint16_t col)
{
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int steps = adx > ady ? adx : ady;
    if (steps == 0) steps = 1;
    for (int i = 0; i <= steps; i++) tft_fill_circle(x0 + dx * i / steps, y0 + dy * i / steps, r, col);
}

static void tft_draw_text(int x, int y, const char *s, int scale, uint16_t col)
{
    if (y + 7 * scale <= s_sy0 || y >= s_sy0 + s_sh) return;
    for (size_t i = 0; s[i] != '\0'; i++) {
        int cx = x + (int)i * 6 * scale;
        if (cx >= s_sx0 + s_sw) break;
        if (cx + 5 * scale < s_sx0) continue;
        uint8_t ch = (uint8_t)s[i];
        if (ch < 0x20 || ch > 0x7E) ch = '?';
        const uint8_t *glyph = kFont5x7[ch - 0x20];
        for (int gx = 0; gx < 5; gx++) {
            uint8_t bits = glyph[gx];
            for (int gy = 0; gy < 7; gy++) {
                if (bits & (1U << gy)) tft_fill_rect(cx + gx * scale, y + gy * scale, scale, scale, col);
            }
        }
    }
}

static void tft_paint_top(const ui_model_t *m)
{
    uint16_t dot = m->wifi_connected ? COL_OK : (m->ap_active ? COL_WARN : COL_ERR);
    tft_fill_circle(8, 8, 3, dot);
    if (m->wifi_connected)      tft_draw_text(15, 5, m->ip, 1, COL_MUTED);
    else if (m->ap_active)      tft_draw_text(15, 5, "AP 192.168.4.1", 1, COL_WARN);
    else                        tft_draw_text(15, 5, "connecting...", 1, COL_MUTED);

    int bx = TFT_W - 24;
    tft_fill_rect(bx, 4, 15, 8, COL_TEXT);
    tft_fill_rect(bx + 1, 5, 13, 6, COL_BG);
    tft_fill_rect(bx + 15, 6, 2, 4, COL_TEXT);
    int fw = (11 * m->battery_pct + 50) / 100;
    if (fw > 11) fw = 11;
    if (fw > 0) tft_fill_rect(bx + 2, 6, fw, 4, m->battery_pct <= 20 ? COL_ERR : COL_OK);

    char btxt[8];
    snprintf(btxt, sizeof(btxt), "%u%%", (unsigned)m->battery_pct);
    tft_draw_text(bx - 4 - text_width(btxt, 1), 5, btxt, 1, COL_MUTED);
}

static void tft_paint_header(const ui_model_t *m)
{
    /* Station Logo (10, 22, 64x64 card with centered 56x56 logo) */
    const int ax = 10, ay = 22, as = 64;
    tft_fill_rect(ax, ay, as, as, COL_WHITE);
    int logo_idx = m->station_index;
    if (logo_idx < 0 || logo_idx >= (int)(sizeof(kStationLogos) / sizeof(kStationLogos[0]))) {
        logo_idx = 0;
    }
    const uint8_t *logo = kStationLogos[logo_idx];
    const int ox = ax + (as - STATION_LOGO_W) / 2;
    const int oy = ay + (as - STATION_LOGO_H) / 2;
    for (int ly = 0; ly < STATION_LOGO_H; ly++) {
        for (int lx = 0; lx < STATION_LOGO_W; lx++) {
            uint8_t bit = (logo[ly * STATION_LOGO_STRIDE + (lx >> 3)] >> (7 - (lx & 7))) & 1U;
            tft_put_pixel(ox + lx, oy + ly, bit ? COL_WHITE : COL_BLACK);
        }
    }

    char meta[64];
    snprintf(meta, sizeof(meta), "%s . #%d/%u", m->genre[0] ? m->genre : "Radio",
             m->station_index + 1, (unsigned)station_list_count());
    if (text_width(meta, 1) > 146) meta[146 / 6] = '\0';
    tft_draw_text(84, 24, meta, 1, COL_MUTED);

    char st_lines[2][48];
    int sc = (text_width(m->station, 2) <= 146) ? 2 : 1;
    int ch = (sc == 2) ? 12 : 24;
    int n = wrap_lines(m->station[0] ? m->station : "ESP32-S3 Radio", ch, 2, st_lines);
    for (int i = 0; i < n; i++) {
        tft_draw_text(84, 38 + i * (sc == 2 ? 18 : 11), st_lines[i], sc, COL_TEXT);
    }
}

static void tft_paint_card(const ui_model_t *m)
{
    const bool playing = (m->playback == PB_PLAYING);
    tft_fill_rect(8, 98, TFT_W - 16, 132, COL_CARD);
    tft_fill_rect(8, 98, TFT_W - 16, 1, COL_CARD_BORDER);
    tft_fill_rect(8, 229, TFT_W - 16, 1, COL_CARD_BORDER);

    const char *st_str = playing ? "Playing" : (m->playback == PB_BUFFERING ? "Buffering" :
                         (m->playback == PB_CONNECTING ? "Connecting" :
                         (m->playback == PB_ERROR ? "Error" : "Stopped")));
    char sub[40];
    snprintf(sub, sizeof(sub), "%s . %s", st_str, m->mono ? "Mono" : "Stereo");
    tft_draw_text(18, 108, sub, 1, COL_MUTED);

    char song[96], artist[48], lines[2][48];
    split_title(m, song, sizeof(song), artist, sizeof(artist));
    int n = wrap_lines(song, 33, 2, lines);
    for (int i = 0; i < n; i++) {
        tft_draw_text(18, 122 + i * 11, lines[i], 1, COL_TEXT);
    }

    const int cy = 172;
    tft_draw_circle(34, cy, 12, COL_CARD_BORDER);
    const char *km = (m->knob_mode == 0) ? "VOL" : "STA";
    tft_draw_text(34 - text_width(km, 1) / 2, cy - 3, km, 1, COL_MUTED);

    tft_fill_rect(76, cy - 4, 2, 9, COL_TEXT);
    tft_draw_line(79, cy, 85, cy - 4, COL_TEXT);
    tft_draw_line(79, cy, 85, cy + 4, COL_TEXT);
    tft_draw_line(85, cy - 4, 85, cy + 4, COL_TEXT);

    tft_fill_circle(120, cy, 16, COL_WHITE);
    if (playing) {
        tft_fill_rect(115, cy - 5, 3, 11, COL_BLACK);
        tft_fill_rect(122, cy - 5, 3, 11, COL_BLACK);
    } else {
        for (int dx = 0; dx < 8; dx++) {
            int half = (7 - dx) / 2;
            tft_fill_rect(117 + dx, cy - half, 1, half * 2 + 1, COL_BLACK);
        }
    }

    tft_draw_line(155, cy - 4, 155, cy + 4, COL_TEXT);
    tft_draw_line(155, cy - 4, 161, cy, COL_TEXT);
    tft_draw_line(155, cy + 4, 161, cy, COL_TEXT);
    tft_fill_rect(162, cy - 4, 2, 9, COL_TEXT);

    char lv[12], rv[12];
    snprintf(lv, sizeof(lv), "%u%%", (unsigned)m->volume);
    if (m->wifi_connected) snprintf(rv, sizeof(rv), "%ddB", m->rssi);
    else                   snprintf(rv, sizeof(rv), "--dB");
    tft_draw_text(18, 206, lv, 1, COL_MUTED);
    int rw = text_width(rv, 1);
    tft_draw_text(TFT_W - 18 - rw, 206, rv, 1, COL_MUTED);
    int bx0 = 48, bw = TFT_W - 24 - rw - bx0;
    if (bw > 20) {
        tft_fill_rect(bx0, 209, bw, 2, COL_TRACK);
        tft_fill_rect(bx0, 209, bw * m->volume / 100, 2, COL_WHITE);
    }
}

static esp_err_t st7789_apply_rotation(uint16_t rot)
{
    switch (rot) {
    case 90:
        ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, true), TAG, "swap");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, true, false), TAG, "mirror");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, 0, 0), TAG, "gap");
        break;
    case 180:
        ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, false), TAG, "swap");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, true, true), TAG, "mirror");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, 0, 80), TAG, "gap");
        break;
    case 270:
        ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, true), TAG, "swap");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, false, true), TAG, "mirror");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, 80, 0), TAG, "gap");
        break;
    default:
        ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, false), TAG, "swap");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, false, false), TAG, "mirror");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, 0, 0), TAG, "gap");
        break;
    }
    return ESP_OK;
}

static void st7789_draw_all(const ui_model_t *m)
{
    render_region(0, 0, TFT_W, 18, COL_BG, tft_paint_top, m);
    render_region(0, 18, TFT_W, 74, COL_BG, tft_paint_header, m);
    render_region(0, 92, TFT_W, 148, COL_BG, tft_paint_card, m);
}

static void st7789_ui_task(void *arg)
{
    (void)arg;
    static ui_model_t cur, last;
    memset(&cur, 0, sizeof(cur));
    model_capture(&cur);
    st7789_draw_all(&cur);
    last = cur;

    for (;;) {
        bool playing = (cur.playback == PB_PLAYING);
        vTaskDelay(pdMS_TO_TICKS(playing ? FRAME_MS_PLAY : FRAME_MS_IDLE));
        int disc = cur.disc_deg;
        model_capture(&cur);
        cur.disc_deg = disc;
        playing = (cur.playback == PB_PLAYING);

        if (cur.rotation != last.rotation) {
            st7789_apply_rotation(cur.rotation);
            st7789_draw_all(&cur);
            last = cur;
            continue;
        }
        if (strcmp(cur.ip, last.ip) || cur.wifi_connected != last.wifi_connected ||
            cur.ap_active != last.ap_active || cur.battery_pct != last.battery_pct) {
            render_region(0, 0, TFT_W, 18, COL_BG, tft_paint_top, &cur);
        }
        if (playing || cur.station_index != last.station_index) {
            if (playing) cur.disc_deg = (cur.disc_deg + DISC_STEP_DEG) % 360;
            render_region(0, 18, TFT_W, 74, COL_BG, tft_paint_header, &cur);
        }
        if (strcmp(cur.title, last.title) || cur.playback != last.playback ||
            cur.volume != last.volume || cur.knob_mode != last.knob_mode ||
            cur.mono != last.mono || cur.rssi / 5 != last.rssi / 5) {
            render_region(0, 92, TFT_W, 148, COL_BG, tft_paint_card, &cur);
        }
        last = cur;
    }
}

static esp_err_t st7789_backend_init(uint16_t init_rot)
{
    s_dma_done = xSemaphoreCreateBinaryStatic(&s_dma_done_buf);
    if (s_dma_done == NULL) return ESP_ERR_NO_MEM;

    gpio_config_t blk_cfg = {
        .pin_bit_mask = (1ULL << APP_EPD_BUSY_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&blk_cfg);
    gpio_set_level(APP_EPD_BUSY_GPIO, 1);

    spi_bus_config_t bus = {
        .mosi_io_num = APP_EPD_MOSI_GPIO,
        .miso_io_num = -1,
        .sclk_io_num = APP_EPD_CLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SURF_PIXELS * (int)sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi_bus_initialize");

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = APP_EPD_CS_GPIO,
        .dc_gpio_num = APP_EPD_DC_GPIO,
        .spi_mode = 0,
        .pclk_hz = 26 * 1000 * 1000,
        .trans_queue_depth = 4,
        .on_color_trans_done = on_color_done,
        .user_ctx = NULL,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &s_io),
                        TAG, "new_panel_io_spi");

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = APP_EPD_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_io, &panel_cfg, &s_panel), TAG, "new_panel_st7789");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "invert");
    ESP_RETURN_ON_ERROR(st7789_apply_rotation(init_rot), TAG, "rot");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "disp on");

    if (xTaskCreate(st7789_ui_task, "tft_ui", 3584, NULL, APP_TASK_PRIO_EPD, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ST7789 240x240 TFT ready on SPI2 (MOSI=%d CLK=%d CS=%d DC=%d RST=%d)",
             APP_EPD_MOSI_GPIO, APP_EPD_CLK_GPIO, APP_EPD_CS_GPIO, APP_EPD_DC_GPIO, APP_EPD_RST_GPIO);
    return ESP_OK;
}

/* ---- public entry point ------------------------------------------------- */
esp_err_t ui_display_init(void)
{
    app_status_t st;
    app_status_get(&st);
    if (st.display_type == DISPLAY_ST7789_240) {
        return st7789_backend_init(st.display_rot);
    }
    return epd_backend_init();
}
