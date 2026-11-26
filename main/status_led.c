/*
 * status_led.c - WS2812 via the espressif/led_strip component (RMT backend).
 *
 * GPIO27 is also a strapping pin on the ESP32-C5; driving it as an output
 * after boot is exactly what the DevKitC-1 does for its RGB LED, so this is
 * safe. Rendering happens in the esp_timer task every APP_RGB_TICK_MS.
 */
#include "esp_log.h"
#include "esp_timer.h"
#include "led_strip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "app_config.h"
#include "wifi_manager.h"
#include "status_led.h"

static const char *TAG = "led";

static led_strip_handle_t s_strip = NULL;
static SemaphoreHandle_t  s_lock = NULL;
static StaticSemaphore_t  s_lock_buf;
static esp_timer_handle_t s_timer = NULL;

static uint8_t  s_knob_mode = APP_ENC_MODE_DEFAULT;
static bool     s_override = false;
static uint8_t  s_ovr_r, s_ovr_g, s_ovr_b;
static int64_t  s_ovr_until_us = 0;      /* 0 = no expiry */
static uint32_t s_tick = 0;
static uint8_t  s_last_r = 0xFF, s_last_g = 0xFF, s_last_b = 0xFF;

static inline uint32_t scale(uint8_t v)
{
    return ((uint32_t)v * APP_RGB_LED_BRIGHTNESS) / 255U;
}

/* Write the pixel only when the colour changed (RMT transfer otherwise idle). */
static void write_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (s_strip == NULL) {
        return;
    }
    if (r == s_last_r && g == s_last_g && b == s_last_b) {
        return;
    }
    esp_err_t err = led_strip_set_pixel(s_strip, 0, scale(r), scale(g), scale(b));
    if (err == ESP_OK) {
        err = led_strip_refresh(s_strip);
    }
    if (err == ESP_OK) {
        s_last_r = r;
        s_last_g = g;
        s_last_b = b;
    } else {
        ESP_LOGW(TAG, "set pixel: %s", esp_err_to_name(err));
    }
}

static void render(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_override && s_ovr_until_us != 0 && esp_timer_get_time() > s_ovr_until_us) {
        s_override = false;
    }
    if (s_override) {
        write_rgb(s_ovr_r, s_ovr_g, s_ovr_b);
    } else if (wifi_manager_is_connected()) {
        if (s_knob_mode == 0) {
            write_rgb(APP_RGB_VOLUME_R, APP_RGB_VOLUME_G, APP_RGB_VOLUME_B);
        } else {
            write_rgb(APP_RGB_STATION_R, APP_RGB_STATION_G, APP_RGB_STATION_B);
        }
    } else if (wifi_manager_is_ap_active()) {
        /* local / portal mode: slow blink */
        bool on = ((s_tick / APP_RGB_BLINK_SLOW_TICKS) & 1U) == 0;
        if (on) {
            write_rgb(APP_RGB_AP_R, APP_RGB_AP_G, APP_RGB_AP_B);
        } else {
            write_rgb(0, 0, 0);
        }
    } else {
        /* STA connecting / reconnecting: fast blink */
        bool on = ((s_tick / APP_RGB_BLINK_FAST_TICKS) & 1U) == 0;
        if (on) {
            write_rgb(APP_RGB_CONNECTING_R, APP_RGB_CONNECTING_G, APP_RGB_CONNECTING_B);
        } else {
            write_rgb(0, 0, 0);
        }
    }
    xSemaphoreGive(s_lock);
}

static void tick_cb(void *arg)
{
    (void)arg;
    s_tick++;
    render();
}

esp_err_t status_led_init(void)
{
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = APP_RGB_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        },
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,      /* 10 MHz: 0.1 us per RMT tick */
        .mem_block_symbols = 0,                 /* driver default */
        .flags = {
            .with_dma = false,
        },
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device: %s", esp_err_to_name(err));
        s_strip = NULL;
        return err;
    }
    led_strip_clear(s_strip);

    esp_timer_create_args_t targs = {
        .callback = tick_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "led_tick",
        .skip_unhandled_events = true,
    };
    err = esp_timer_create(&targs, &s_timer);
    if (err == ESP_OK) {
        err = esp_timer_start_periodic(s_timer, (uint64_t)APP_RGB_TICK_MS * 1000);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "tick timer: %s (LED static only)", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "RGB LED ready on GPIO%d (WS2812, brightness %d/255): AP=magenta blink, "
             "connecting=yellow blink, connected=knob colour", APP_RGB_LED_GPIO, APP_RGB_LED_BRIGHTNESS);
    return ESP_OK;
}

void status_led_set_knob_mode(uint8_t mode)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_knob_mode = mode;
    xSemaphoreGive(s_lock);
    render();
}

void status_led_set_override(uint8_t r, uint8_t g, uint8_t b, uint32_t ms)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_override = true;
    s_ovr_r = r;
    s_ovr_g = g;
    s_ovr_b = b;
    s_ovr_until_us = ms ? esp_timer_get_time() + (int64_t)ms * 1000 : 0;
    xSemaphoreGive(s_lock);
    render();
}

void status_led_clear_override(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_override = false;
    xSemaphoreGive(s_lock);
    render();
}

void status_led_off(void)
{
    if (s_strip == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    led_strip_clear(s_strip);
    s_last_r = s_last_g = s_last_b = 0;
    xSemaphoreGive(s_lock);
}
