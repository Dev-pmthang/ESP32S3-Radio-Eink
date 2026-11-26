/*
 * main.c - ESP32-C5 CYBER WEB RADIO entry point.
 *
 * Boot order matters for zero-pop:
 *   1. XSMT LOW (hardware mute) before anything else.
 *   2. NVS, event loop, status.
 *   3. I2S channel (preloaded with zeros, disabled), stream buffer, decoder,
 *      command manager, encoder.
 *   4. Wi-Fi, then the web server.
 *   5. Low priority monitor task: heap, stack high-water marks, CPU usage.
 */
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "sdkconfig.h"

#include "app_config.h"
#include "app_status.h"
#include "audio_player.h"
#include "audio_decoder.h"
#include "stream_manager.h"
#include "command_manager.h"
#include "wifi_manager.h"
#include "rotary_encoder.h"
#include "status_led.h"
#include "ui_display.h"
#include "ota_update.h"
#include "web_server.h"

static const char *TAG = "main";

static void hardware_mute_asap(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << APP_DAC_XSMT_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(APP_DAC_XSMT_GPIO, 0);
}

static esp_err_t nvs_init_safe(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erase (%s)", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            return err;
        }
        err = nvs_flash_init();
    }
    return err;
}

static void on_wifi_state(bool connected)
{
    static bool autoplayed = false;
    ESP_LOGI(TAG, "Wi-Fi %s", connected ? "connected" : "disconnected");
#if APP_AUTOPLAY_ON_CONNECT
    if (connected && !autoplayed) {
        autoplayed = true;
        command_manager_post_simple(CMD_RESUME, 0);
    }
#endif
}

static void monitor_task(void *arg)
{
    (void)arg;
#if CONFIG_FREERTOS_USE_TRACE_FACILITY
    static TaskStatus_t tasks[24];
#endif
    uint8_t sim_bat = 92;
    uint8_t bat_ticks = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(APP_MONITOR_PERIOD_MS));
        if (++bat_ticks >= 3) {             /* simulated battery: -1% every 30 s, recharges at 15% */
            bat_ticks = 0;
            sim_bat = (sim_bat > 15) ? (uint8_t)(sim_bat - 1) : 100;
            app_status_set_battery(sim_bat);
        }
        /* First tick after boot: the app has run 10 s without crashing, so a
         * freshly OTA-updated image is confirmed (cancels bootloader rollback). */
        ota_update_mark_valid_if_pending();
        ESP_LOGI(TAG, "heap free %" PRIu32 " B, min ever %" PRIu32 " B, DMA underruns %" PRIu32,
                 esp_get_free_heap_size(), esp_get_minimum_free_heap_size(),
                 audio_player_get_dma_underruns());
#if CONFIG_FREERTOS_USE_TRACE_FACILITY
        configRUN_TIME_COUNTER_TYPE total_runtime = 0;
        UBaseType_t n = uxTaskGetSystemState(tasks, sizeof(tasks) / sizeof(tasks[0]), &total_runtime);
        for (UBaseType_t i = 0; i < n; i++) {
            const char *name = tasks[i].pcTaskName;
            bool ours = strcmp(name, "audio_decoder") == 0 || strcmp(name, "http_stream") == 0 ||
                        strcmp(name, "cmd_mgr") == 0 || strcmp(name, "encoder") == 0 ||
                        strcmp(name, "httpd") == 0 || strcmp(name, "IDLE") == 0 ||
                        strcmp(name, "IDLE0") == 0 || strcmp(name, "IDLE1") == 0 ||
                        strcmp(name, "epd_ui") == 0 || strcmp(name, "captive_dns") == 0;
            if (!ours) {
                continue;
            }
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
            unsigned pct = total_runtime ? (unsigned)((uint64_t)tasks[i].ulRunTimeCounter * 100 / total_runtime) : 0;
            ESP_LOGI(TAG, "  %-14s prio %2u  stack HWM %5u B  cpu %3u%%",
                     name, (unsigned)tasks[i].uxCurrentPriority,
                     (unsigned)tasks[i].usStackHighWaterMark, pct);
#else
            ESP_LOGI(TAG, "  %-14s prio %2u  stack HWM %5u B",
                     name, (unsigned)tasks[i].uxCurrentPriority,
                     (unsigned)tasks[i].usStackHighWaterMark);
#endif
        }
#endif
    }
}

void app_main(void)
{
    hardware_mute_asap();

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "ESP32-S3 CYBER WEB RADIO | IDF %s | cores %d | CPU %d MHz",
             esp_get_idf_version(), chip.cores, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);

    /* Unrecoverable failures below: without NVS/event loop/I2S nothing works. */
    ESP_ERROR_CHECK(nvs_init_safe());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(app_status_init());
    ESP_ERROR_CHECK(audio_player_init());
    ESP_ERROR_CHECK(stream_manager_init());
    ESP_ERROR_CHECK(audio_decoder_init());
    ESP_ERROR_CHECK(command_manager_init());

    esp_err_t err = status_led_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RGB LED unavailable: %s", esp_err_to_name(err));
    }

    err = rotary_encoder_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "rotary encoder unavailable: %s (web control still works)", esp_err_to_name(err));
    }

    ESP_ERROR_CHECK(wifi_manager_init(on_wifi_state));
    ESP_ERROR_CHECK(wifi_manager_start());

    err = web_server_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "web server failed: %s (encoder control still works)", esp_err_to_name(err));
    }

#if APP_EPD_ENABLE
    err = ui_display_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "E-Ink display unavailable: %s (radio keeps running)", esp_err_to_name(err));
    }
#endif

    if (xTaskCreate(monitor_task, "monitor", APP_TASK_STACK_MONITOR, NULL, 1, NULL) != pdPASS) {
        ESP_LOGW(TAG, "monitor task not started");
    }

    ESP_LOGI(TAG, "boot complete, heap free %" PRIu32 " B", esp_get_free_heap_size());
    if (!wifi_manager_has_credentials()) {
        ESP_LOGI(TAG, "no Wi-Fi credentials: join SoftAP '%s' and open http://192.168.4.1/", APP_WIFI_AP_SSID);
    }
}
