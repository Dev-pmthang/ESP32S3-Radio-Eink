/*
 * rotary_encoder.c - KY-040 on GPIO1 (CLK), GPIO2 (DT), GPIO3 (SW).
 *
 * Knob:
 *   rotate, mode VOLUME  : one detent = +/- APP_VOLUME_STEP percent
 *   rotate, mode STATION : detents accumulate; applied as one station change
 *                          once the knob is still for APP_ENC_STATION_SETTLE_MS
 *   short press (release < APP_ENC_LONGPRESS_MS) : toggle VOLUME / STATION
 *   hold >= APP_ENC_LONGPRESS_WARN_MS            : LED turns red (warning)
 *   hold >= APP_ENC_LONGPRESS_MS                 : erase Wi-Fi credentials,
 *                                                  LED white, restart to portal
 *
 * ISR: reads the inputs, timestamps with esp_timer_get_time() (ISR-safe) and
 *      queues the sample with xQueueSendFromISR(). Nothing else happens in
 *      interrupt context.
 * Task: Gray-code transition table decoding (rejects invalid transitions
 *      caused by bounce), one step per detent when both lines are back at
 *      rest, 4 ms minimum spacing between detents, 250 ms between presses.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"

#include "app_config.h"
#include "app_status.h"
#include "station_list.h"
#include "command_manager.h"
#include "status_led.h"
#include "rotary_encoder.h"

static const char *TAG = "encoder";

typedef struct {
    int64_t ts_us;
    uint8_t pins;       /* rotary: bit1 = CLK, bit0 = DT; button: bit0 = SW level */
    uint8_t is_button;
} enc_event_t;

static StaticQueue_t  s_queue_buf;
static uint8_t        s_queue_storage[APP_ENC_RAW_QUEUE_LEN * sizeof(enc_event_t)];
static QueueHandle_t  s_queue = NULL;
static TaskHandle_t   s_task = NULL;
static volatile uint32_t s_isr_dropped = 0;

/* Valid quadrature transitions: index = (prev << 2) | cur, value = direction. */
static const int8_t s_transition[16] = {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0,
};

static inline uint8_t read_pins(void)
{
    return (uint8_t)((gpio_get_level(APP_ENC_CLK_GPIO) << 1) | gpio_get_level(APP_ENC_DT_GPIO));
}

static void IRAM_ATTR rotary_isr(void *arg)
{
    (void)arg;
    enc_event_t ev = {
        .ts_us = esp_timer_get_time(),
        .pins = read_pins(),
        .is_button = 0,
    };
    BaseType_t hp_woken = pdFALSE;
    if (xQueueSendFromISR(s_queue, &ev, &hp_woken) != pdTRUE) {
        s_isr_dropped++;
    }
    if (hp_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void IRAM_ATTR button_isr(void *arg)
{
    (void)arg;
    enc_event_t ev = {
        .ts_us = esp_timer_get_time(),
        .pins = (uint8_t)gpio_get_level(APP_ENC_SW_GPIO),
        .is_button = 1,
    };
    BaseType_t hp_woken = pdFALSE;
    if (xQueueSendFromISR(s_queue, &ev, &hp_woken) != pdTRUE) {
        s_isr_dropped++;
    }
    if (hp_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/* Apply the accumulated station delta as a single PLAY_STATION command. */
static void flush_station_delta(int *pending)
{
    int count = (int)station_list_count();
    if (*pending == 0 || count == 0) {
        *pending = 0;
        return;
    }
    int current = command_manager_get_station();
    int target = ((current + *pending) % count + count) % count;
    *pending = 0;
    if (target == current) {
        return;
    }
    const station_t *st = station_list_get((size_t)target);
    ESP_LOGI(TAG, "knob -> station %d/%d: %s", target + 1, count, st ? st->name : "?");
    command_manager_post_simple(CMD_PLAY_STATION, target);
}

static void encoder_task(void *arg)
{
    (void)arg;
    uint8_t prev = read_pins();
    int     acc = 0;
    int64_t last_detent_us = 0;
    int     local_volume = command_manager_get_volume();
    int     mode = APP_ENC_MODE_DEFAULT;          /* 0 = volume, 1 = station */
    int     pending_station = 0;
    int64_t last_station_turn_us = 0;

    /* button state */
    bool    btn_down = false;
    int64_t btn_down_us = 0;
    bool    btn_warned = false;
    bool    btn_long_done = false;
    enc_event_t ev;

    app_status_set_encoder_mode((uint8_t)mode);
    status_led_set_knob_mode((uint8_t)mode);

    for (;;) {
        /* Short receive timeout while something time-based is pending
         * (station settle, long-press hold); otherwise block indefinitely. */
        TickType_t wait = (pending_station != 0 || btn_down) ? pdMS_TO_TICKS(50) : portMAX_DELAY;
        bool got = xQueueReceive(s_queue, &ev, wait) == pdTRUE;
        int64_t now = esp_timer_get_time();

        if (pending_station != 0 && !btn_down &&
            (now - last_station_turn_us) >= (int64_t)APP_ENC_STATION_SETTLE_MS * 1000) {
            flush_station_delta(&pending_station);
        }

        /* Long-press tracking while the button is held. */
        if (btn_down && gpio_get_level(APP_ENC_SW_GPIO) == 0) {
            int64_t held_ms = (now - btn_down_us) / 1000;
            if (!btn_warned && held_ms >= APP_ENC_LONGPRESS_WARN_MS) {
                btn_warned = true;
                status_led_set_override(APP_RGB_HOLD_R, APP_RGB_HOLD_G, APP_RGB_HOLD_B, 0);
                ESP_LOGI(TAG, "hold... release now to keep Wi-Fi, keep holding to erase");
            }
            if (!btn_long_done && held_ms >= APP_ENC_LONGPRESS_MS) {
                btn_long_done = true;
                status_led_set_override(255, 255, 255, 0);  /* white until restart */
                ESP_LOGW(TAG, "long press: erase Wi-Fi credentials and restart into portal");
                command_manager_post_simple(CMD_WIFI_RESET, 0);
            }
        }

        /* Button: on any edge (or on the 50 ms poll while held) wait for the
         * contact to settle, then reconcile the STABLE level with our state.
         * One code path handles press and release, so bounce order and a
         * missed edge cannot desynchronise it. */
        bool check_button = (got && ev.is_button) || (!got && btn_down);
        if (check_button) {
            if (got) {
                vTaskDelay(pdMS_TO_TICKS(20));          /* debounce settle */
            }
            bool pressed = gpio_get_level(APP_ENC_SW_GPIO) == 0;    /* active low */
            int64_t t = esp_timer_get_time();
            if (pressed && !btn_down) {
                btn_down = true;
                btn_down_us = t;
                btn_warned = false;
                btn_long_done = false;
            } else if (!pressed && btn_down) {
                btn_down = false;
                int64_t held_ms = (t - btn_down_us) / 1000;
                status_led_clear_override();
                if (btn_long_done) {
                    /* reset already issued, device is about to restart */
                } else if (held_ms >= APP_ENC_LONGPRESS_WARN_MS) {
                    ESP_LOGI(TAG, "hold released after %lld ms: no action", (long long)held_ms);
                } else {
                    /* short press: toggle knob mode */
                    flush_station_delta(&pending_station);
                    mode = (mode == 0) ? 1 : 0;
                    app_status_set_encoder_mode((uint8_t)mode);
                    status_led_set_knob_mode((uint8_t)mode);
                    ESP_LOGI(TAG, "knob mode -> %s (press %lld ms)", mode == 0 ? "VOLUME" : "STATION",
                             (long long)held_ms);
                }
            }
            if (got) {
                continue;
            }
        }

        if (!got) {
            continue;
        }

        uint8_t cur = ev.pins & 0x03;
        int8_t step = s_transition[(prev << 2) | cur];
        prev = cur;
        if (step != 0) {
            acc += step;
        }
        if (cur != 0x03) {
            continue;       /* not at the detent rest position (both high) */
        }
        int dir = 0;
        if (acc >= 2) {
            dir = 1;
        } else if (acc <= -2) {
            dir = -1;
        }
        acc = 0;
        if (dir == 0) {
            continue;
        }
        if ((ev.ts_us - last_detent_us) < APP_ENC_DEBOUNCE_US) {
            continue;
        }
        bool idle_before = (ev.ts_us - last_detent_us) > (int64_t)APP_ENC_RESYNC_MS * 1000;
        last_detent_us = ev.ts_us;
#if APP_ENC_INVERT_DIR
        dir = -dir;
#endif
        if (mode == 1) {
            pending_station += dir;
            last_station_turn_us = ev.ts_us;
            int count = (int)station_list_count();
            if (count > 0) {
                int preview = ((command_manager_get_station() + pending_station) % count + count) % count;
                const station_t *st = station_list_get((size_t)preview);
                ESP_LOGI(TAG, "knob %s -> preview %d/%d: %s", dir > 0 ? "CW" : "CCW", preview + 1, count,
                         st ? st->name : "?");
            }
            continue;
        }

        if (idle_before) {
            local_volume = command_manager_get_volume();    /* pick up web changes */
        }
        local_volume += dir * APP_VOLUME_STEP;
        if (local_volume < 0) {
            local_volume = 0;
        }
        if (local_volume > 100) {
            local_volume = 100;
        }
        command_manager_post_simple(CMD_SET_VOLUME, local_volume);
    }
}

esp_err_t rotary_encoder_init(void)
{
    s_queue = xQueueCreateStatic(APP_ENC_RAW_QUEUE_LEN, sizeof(enc_event_t), s_queue_storage, &s_queue_buf);
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    gpio_config_t rot = {
        .pin_bit_mask = (1ULL << APP_ENC_CLK_GPIO) | (1ULL << APP_ENC_DT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    esp_err_t err = gpio_config(&rot);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rotary gpio_config: %s", esp_err_to_name(err));
        return err;
    }
    gpio_config_t btn = {
        .pin_bit_mask = (1ULL << APP_ENC_SW_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,         /* both press and release */
    };
    err = gpio_config(&btn);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "button gpio_config: %s", esp_err_to_name(err));
        return err;
    }

    /* Default (non-IRAM) ISR service: handlers are deferred while the flash
     * cache is disabled, so they may call ordinary driver functions. */
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "isr service: %s", esp_err_to_name(err));
        return err;
    }
    err = gpio_isr_handler_add(APP_ENC_CLK_GPIO, rotary_isr, NULL);
    if (err == ESP_OK) {
        err = gpio_isr_handler_add(APP_ENC_DT_GPIO, rotary_isr, NULL);
    }
    if (err == ESP_OK) {
        err = gpio_isr_handler_add(APP_ENC_SW_GPIO, button_isr, NULL);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "isr add: %s", esp_err_to_name(err));
        return err;
    }

    BaseType_t ok = xTaskCreate(encoder_task, "encoder", APP_TASK_STACK_ENCODER, NULL,
                                APP_TASK_PRIO_ENCODER, &s_task);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "KY-040 ready: CLK=%d DT=%d SW=%d (press = mode, hold %d s = Wi-Fi reset)",
             APP_ENC_CLK_GPIO, APP_ENC_DT_GPIO, APP_ENC_SW_GPIO, APP_ENC_LONGPRESS_MS / 1000);
    return ESP_OK;
}
