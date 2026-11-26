/*
 * command_manager.c - sequential command execution + playback state machine.
 *
 *  web / encoder / wifi ---> queue ---> command task ---> stream_manager
 *                                                   \--> audio_decoder
 *                                                   \--> wifi_manager
 *                                                   \--> NVS (settings)
 *
 * Priority: CMD_STOP and station changes are inserted at the FRONT of the
 * queue. Volume updates are coalesced into one pending value so a fast
 * encoder spin cannot overflow the queue. State based de-duplication
 * (STOP while stopped, PLAY same station while playing, RESUME while
 * playing) makes repeated identical commands harmless.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "app_config.h"
#include "app_status.h"
#include "station_list.h"
#include "stream_manager.h"
#include "audio_decoder.h"
#include "wifi_manager.h"
#include "command_manager.h"

static const char *TAG = "cmd";

static StaticQueue_t        s_queue_buf;
static uint8_t              s_queue_storage[APP_CMD_QUEUE_LEN * sizeof(app_cmd_t)];
static QueueHandle_t        s_queue = NULL;
static TaskHandle_t         s_task = NULL;
static portMUX_TYPE         s_spin = portMUX_INITIALIZER_UNLOCKED;

static volatile uint8_t     s_volume = APP_VOLUME_DEFAULT;
static volatile int         s_station = 0;
static bool                 s_mono = (APP_AUDIO_MONO_DEFAULT != 0);
static display_type_t       s_disp_type = DISPLAY_EPD_154;
static uint16_t             s_disp_rot = APP_EPD_ROTATION;
static volatile int32_t     s_pending_volume = -1;     /* -1 = nothing pending */
static volatile uint32_t    s_dropped = 0;
static bool                 s_playing = false;
static esp_timer_handle_t   s_save_timer = NULL;

/* ---- settings (NVS namespace app_cfg) ----------------------------------- */
static void settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open(APP_SETTINGS_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no saved settings, using defaults");
        return;
    }
    uint8_t v;
    if (nvs_get_u8(h, "volume", &v) == ESP_OK && v <= 100) {
        s_volume = v;
    }
    if (nvs_get_u8(h, "station", &v) == ESP_OK && v < station_list_count()) {
        s_station = v;
    }
    if (nvs_get_u8(h, "mono", &v) == ESP_OK) {
        s_mono = (v != 0);
    }
    if (nvs_get_u8(h, "disp_type", &v) == ESP_OK && v < DISPLAY_TYPE_MAX) {
        s_disp_type = (display_type_t)v;
    }
    if (nvs_get_u8(h, "disp_rot", &v) == ESP_OK && v < 4) {
        s_disp_rot = (uint16_t)(v * 90U);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "settings: volume %u%%, station %d, %s, disp %s rot %u",
             (unsigned)s_volume, s_station, s_mono ? "mono downmix" : "stereo",
             display_type_str(s_disp_type), (unsigned)s_disp_rot);
}

static void settings_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(APP_SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_u8(h, "volume", s_volume);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "station", (uint8_t)s_station);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "mono", s_mono ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "disp_type", (uint8_t)s_disp_type);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "disp_rot", (uint8_t)(s_disp_rot / 90U));
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "settings save failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGD(TAG, "settings saved");
    }
}

static void save_timer_cb(void *arg)
{
    (void)arg;
    /* Keep NVS writes out of the esp_timer task: hand over to the command task. */
    command_manager_post_simple(CMD_SAVE_SETTINGS, 0);
}

static void schedule_save(void)
{
    if (s_save_timer == NULL) {
        return;
    }
    esp_timer_stop(s_save_timer);   /* ESP_ERR_INVALID_STATE when not running: harmless */
    esp_timer_start_once(s_save_timer, (uint64_t)APP_SETTINGS_SAVE_DELAY_MS * 1000);
}

/* ---- pipeline control --------------------------------------------------- */
static void pipeline_stop(void)
{
    /* Decoder first: it fades out and hard-mutes before the stream dies. */
    esp_err_t e1 = audio_decoder_stop(APP_PIPELINE_STOP_TIMEOUT_MS);
    esp_err_t e2 = stream_manager_stop(APP_PIPELINE_STOP_TIMEOUT_MS);
    if (e1 == ESP_OK && e2 == ESP_OK) {
        esp_err_t e3 = stream_manager_reset_buffer();
        if (e3 != ESP_OK) {
            ESP_LOGW(TAG, "buffer reset: %s", esp_err_to_name(e3));
        }
    } else {
        ESP_LOGE(TAG, "pipeline stop incomplete (decoder %s, stream %s)",
                 esp_err_to_name(e1), esp_err_to_name(e2));
    }
    s_playing = false;
}

static void pipeline_start(int index)
{
    const station_t *st = station_list_get((size_t)index);
    if (st == NULL) {
        ESP_LOGE(TAG, "invalid station %d", index);
        return;
    }
    pipeline_stop();
    s_station = index;
    app_status_set_station(index, st->name, st->genre);
    app_status_set_playback(PB_CONNECTING);
    esp_err_t err = stream_manager_start(st->url);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stream start: %s", esp_err_to_name(err));
        app_status_set_error("stream start failed");
        return;
    }
    err = audio_decoder_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "decoder start: %s", esp_err_to_name(err));
        app_status_set_error("decoder start failed");
        stream_manager_stop(APP_PIPELINE_STOP_TIMEOUT_MS);
        return;
    }
    s_playing = true;
    ESP_LOGI(TAG, "playing [%d] %s", index, st->name);
    schedule_save();
}

static void apply_volume(int32_t v)
{
    if (v < 0) {
        v = 0;
    }
    if (v > 100) {
        v = 100;
    }
    if ((uint8_t)v == s_volume) {
        return;
    }
    s_volume = (uint8_t)v;
    audio_decoder_set_volume(s_volume);
    ESP_LOGI(TAG, "volume %u%%", (unsigned)s_volume);
    schedule_save();
}

static void handle_command(const app_cmd_t *cmd)
{
    int count = (int)station_list_count();
    switch (cmd->type) {
    case CMD_PLAY_STATION: {
        int idx = cmd->arg < 0 ? s_station : cmd->arg;
        if (idx < 0 || idx >= count) {
            ESP_LOGW(TAG, "PLAY: station %d out of range", idx);
            return;
        }
        if (s_playing && idx == s_station && app_status_get_playback() != PB_ERROR) {
            ESP_LOGD(TAG, "PLAY: already on station %d", idx);
            return;
        }
        pipeline_start(idx);
        break;
    }
    case CMD_NEXT_STATION:
        pipeline_start((s_station + 1) % count);
        break;
    case CMD_PREVIOUS_STATION:
        pipeline_start((s_station + count - 1) % count);
        break;
    case CMD_STOP:
        if (!s_playing) {
            ESP_LOGD(TAG, "STOP: already stopped");
            return;
        }
        pipeline_stop();
        app_status_set_playback(PB_STOPPED);
        ESP_LOGI(TAG, "stopped");
        break;
    case CMD_RESUME:
        if (s_playing && app_status_get_playback() != PB_ERROR) {
            return;
        }
        pipeline_start(s_station);
        break;
    case CMD_SET_VOLUME: {
        int32_t v;
        taskENTER_CRITICAL(&s_spin);
        v = s_pending_volume;
        s_pending_volume = -1;
        taskEXIT_CRITICAL(&s_spin);
        if (v < 0) {
            v = cmd->arg;
        }
        if (v >= 0) {
            apply_volume(v);
        }
        break;
    }
    case CMD_WIFI_CONFIG: {
        esp_err_t err = wifi_manager_set_credentials(cmd->ssid, cmd->password);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "wifi config: %s", esp_err_to_name(err));
        }
        break;
    }
    case CMD_SET_MONO: {
        bool mono = cmd->arg != 0;
        if (mono != s_mono) {
            s_mono = mono;
            audio_decoder_set_mono(mono);
            ESP_LOGI(TAG, "output: %s", mono ? "mono downmix (L+R)/2" : "stereo");
            schedule_save();
        }
        break;
    }
    case CMD_SET_DISPLAY: {
        uint16_t type_raw = (uint16_t)((cmd->arg >> 16) & 0xFFFF);
        uint16_t rot_raw = (uint16_t)(cmd->arg & 0xFFFF);
        bool type_changed = (type_raw != 0xFFFF && type_raw < DISPLAY_TYPE_MAX &&
                             (display_type_t)type_raw != s_disp_type);
        bool rot_changed = (rot_raw != 0xFFFF &&
                            (rot_raw == 0 || rot_raw == 90 || rot_raw == 180 || rot_raw == 270) &&
                            rot_raw != s_disp_rot);
        if (rot_changed) {
            s_disp_rot = rot_raw;
            ESP_LOGI(TAG, "display rotation: %u deg", (unsigned)s_disp_rot);
        }
        if (type_changed) {
            s_disp_type = (display_type_t)type_raw;
            ESP_LOGW(TAG, "display type changed to %s, saving and restarting",
                     display_type_str(s_disp_type));
            app_status_set_display(s_disp_type, s_disp_rot);
            settings_save();
            pipeline_stop();
            vTaskDelay(pdMS_TO_TICKS(400));
            esp_restart();
        } else if (rot_changed) {
            app_status_set_display(s_disp_type, s_disp_rot);
            schedule_save();
        }
        break;
    }
    case CMD_WIFI_RESET: {
        ESP_LOGW(TAG, "Wi-Fi reset requested: stopping audio, erasing credentials, restarting");
        pipeline_stop();                 /* XSMT LOW first: no pop on reboot */
        settings_save();                 /* keep volume/station/mono */
        esp_err_t err = wifi_manager_clear_credentials();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "clear credentials: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(500));  /* let the LED flash and logs flush */
        esp_restart();
        break;
    }
    case CMD_SAVE_SETTINGS:
        settings_save();
        break;
    default:
        ESP_LOGW(TAG, "unknown command %d", (int)cmd->type);
        break;
    }
}

static void command_task(void *arg)
{
    (void)arg;
    app_cmd_t cmd;
    for (;;) {
        if (xQueueReceive(s_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            handle_command(&cmd);
        }
    }
}

/* ---- public API --------------------------------------------------------- */
static inline bool is_priority_cmd(app_cmd_type_t t)
{
    return t == CMD_STOP || t == CMD_NEXT_STATION || t == CMD_PREVIOUS_STATION || t == CMD_PLAY_STATION ||
           t == CMD_WIFI_RESET;
}

esp_err_t command_manager_init(void)
{
    s_queue = xQueueCreateStatic(APP_CMD_QUEUE_LEN, sizeof(app_cmd_t), s_queue_storage, &s_queue_buf);
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    settings_load();
    audio_decoder_set_volume(s_volume);
    audio_decoder_set_mono(s_mono);
    app_status_set_display(s_disp_type, s_disp_rot);

    const station_t *st = station_list_get((size_t)s_station);
    if (st != NULL) {
        app_status_set_station(s_station, st->name, st->genre);
    }

    esp_timer_create_args_t targs = {
        .callback = save_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "cfg_save",
        .skip_unhandled_events = true,
    };
    esp_err_t err = esp_timer_create(&targs, &s_save_timer);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save timer: %s (settings will not persist)", esp_err_to_name(err));
        s_save_timer = NULL;
    }

    BaseType_t ok = xTaskCreate(command_task, "cmd_mgr", APP_TASK_STACK_CMD, NULL, APP_TASK_PRIO_CMD, &s_task);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t command_manager_post(const app_cmd_t *cmd)
{
    if (cmd == NULL || cmd->type >= CMD_TYPE_MAX || s_queue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    app_cmd_t local = *cmd;
    if (local.type == CMD_SET_VOLUME) {
        int32_t v = local.arg < 0 ? 0 : (local.arg > 100 ? 100 : local.arg);
        bool need_enqueue;
        taskENTER_CRITICAL(&s_spin);
        need_enqueue = (s_pending_volume < 0);
        s_pending_volume = v;
        taskEXIT_CRITICAL(&s_spin);
        if (!need_enqueue) {
            return ESP_OK;      /* coalesced into the pending marker */
        }
        local.arg = -1;
    }
    BaseType_t ok = is_priority_cmd(local.type) ? xQueueSendToFront(s_queue, &local, 0)
                                                : xQueueSend(s_queue, &local, 0);
    if (ok != pdTRUE) {
        s_dropped++;
        if (local.type == CMD_SET_VOLUME) {
            taskENTER_CRITICAL(&s_spin);
            s_pending_volume = -1;
            taskEXIT_CRITICAL(&s_spin);
        }
        ESP_LOGW(TAG, "queue full, dropped command %d (total %lu)", (int)local.type, (unsigned long)s_dropped);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t command_manager_post_simple(app_cmd_type_t type, int32_t arg)
{
    app_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = type;
    cmd.arg = arg;
    return command_manager_post(&cmd);
}

BaseType_t command_manager_post_from_isr(const app_cmd_t *cmd, BaseType_t *hp_task_woken)
{
    if (cmd == NULL || cmd->type >= CMD_TYPE_MAX || s_queue == NULL) {
        return pdFALSE;
    }
    app_cmd_t local = *cmd;
    if (local.type == CMD_SET_VOLUME) {
        int32_t v = local.arg < 0 ? 0 : (local.arg > 100 ? 100 : local.arg);
        bool need_enqueue;
        taskENTER_CRITICAL_ISR(&s_spin);
        need_enqueue = (s_pending_volume < 0);
        s_pending_volume = v;
        taskEXIT_CRITICAL_ISR(&s_spin);
        if (!need_enqueue) {
            return pdTRUE;
        }
        local.arg = -1;
    }
    BaseType_t ok = is_priority_cmd(local.type) ? xQueueSendToFrontFromISR(s_queue, &local, hp_task_woken)
                                                : xQueueSendFromISR(s_queue, &local, hp_task_woken);
    if (ok != pdTRUE) {
        s_dropped++;
        if (local.type == CMD_SET_VOLUME) {
            taskENTER_CRITICAL_ISR(&s_spin);
            s_pending_volume = -1;
            taskEXIT_CRITICAL_ISR(&s_spin);
        }
    }
    return ok;
}

uint8_t command_manager_get_volume(void)
{
    return s_volume;
}

int command_manager_get_station(void)
{
    return s_station;
}

uint32_t command_manager_get_dropped_count(void)
{
    return s_dropped;
}
