#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "cJSON.h"

#include "app_config.h"
#include "app_status.h"
#include "command_manager.h"
#include "ota_update.h"

static const char *TAG = "ota";

#define OTA_CHUNK           4096
#define OTA_MIN_IMAGE       (64 * 1024)
#define OTA_STOP_WAIT_MS    3000
#define OTA_RESTART_DELAY_MS 1500

static uint8_t          s_chunk[OTA_CHUNK];     /* static: keep it off the httpd stack */
static volatile bool    s_in_progress = false;
static esp_timer_handle_t s_restart_timer = NULL;

static void restart_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "restarting into the new firmware");
    esp_restart();
}

static esp_err_t send_json(httpd_req_t *req, const char *status, cJSON *root)
{
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (txt == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json alloc");
    }
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, txt, HTTPD_RESP_USE_STRLEN);
    cJSON_free(txt);
    return err;
}

static esp_err_t send_result(httpd_req_t *req, bool ok, const char *status, const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json alloc");
    }
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, ok ? "message" : "error", msg);
    return send_json(req, status, root);
}

esp_err_t ota_update_handle_info(httpd_req_t *req)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (running != NULL) {
        esp_ota_get_state_partition(running, &state);
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json alloc");
    }
    cJSON_AddStringToObject(root, "version", desc->version);
    cJSON_AddStringToObject(root, "project", desc->project_name);
    cJSON_AddStringToObject(root, "idf", desc->idf_ver);
    char built[40];
    snprintf(built, sizeof(built), "%s %s", desc->date, desc->time);
    cJSON_AddStringToObject(root, "built", built);
    cJSON_AddStringToObject(root, "running_partition", running ? running->label : "?");
    cJSON_AddStringToObject(root, "update_partition", next ? next->label : "none");
    cJSON_AddNumberToObject(root, "update_partition_size", next ? (double)next->size : 0);
    cJSON_AddBoolToObject(root, "pending_verify", state == ESP_OTA_IMG_PENDING_VERIFY);
    cJSON_AddBoolToObject(root, "in_progress", s_in_progress);
    return send_json(req, "200 OK", root);
}

/* Ask the command manager to stop and wait until the pipeline is idle. */
static bool stop_playback_for_update(void)
{
    command_manager_post_simple(CMD_STOP, 0);
    for (int i = 0; i < OTA_STOP_WAIT_MS / 50; i++) {
        if (app_status_get_playback() == PB_STOPPED) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return app_status_get_playback() == PB_STOPPED;
}

esp_err_t ota_update_handle_upload(httpd_req_t *req)
{
    if (s_in_progress) {
        return send_result(req, false, "409 Conflict", "update already in progress");
    }
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL) {
        return send_result(req, false, "500 Internal Server Error", "no OTA partition");
    }
    size_t total = req->content_len;
    if (total < OTA_MIN_IMAGE || total > target->size) {
        return send_result(req, false, "400 Bad Request", "image size out of range for the OTA slot");
    }
    s_in_progress = true;
    ESP_LOGW(TAG, "update of %u bytes -> %s", (unsigned)total, target->label);

    if (!stop_playback_for_update()) {
        ESP_LOGW(TAG, "playback did not stop in time, continuing anyway");
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(target, total, &handle);
    if (err != ESP_OK) {
        s_in_progress = false;
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        return send_result(req, false, "500 Internal Server Error", esp_err_to_name(err));
    }

    size_t received = 0;
    int timeouts = 0;
    bool first = true;
    while (received < total) {
        size_t want = total - received;
        if (want > sizeof(s_chunk)) {
            want = sizeof(s_chunk);
        }
        int n = httpd_req_recv(req, (char *)s_chunk, want);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 10) {
                err = ESP_ERR_TIMEOUT;
                break;
            }
            continue;
        }
        if (n <= 0) {
            err = ESP_FAIL;
            break;
        }
        timeouts = 0;
        if (first) {
            first = false;
            if (s_chunk[0] != 0xE9) {           /* ESP image magic byte */
                err = ESP_ERR_INVALID_ARG;
                ESP_LOGE(TAG, "not an ESP application image (magic 0x%02x)", s_chunk[0]);
                break;
            }
        }
        err = esp_ota_write(handle, s_chunk, (size_t)n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            break;
        }
        received += (size_t)n;
        if ((received & 0x3FFFF) < (size_t)n) {        /* every ~256 KB */
            ESP_LOGI(TAG, "%u / %u bytes", (unsigned)received, (unsigned)total);
        }
    }

    if (err != ESP_OK) {
        esp_ota_abort(handle);
        s_in_progress = false;
        command_manager_post_simple(CMD_RESUME, 0);
        return send_result(req, false, "400 Bad Request",
                           err == ESP_ERR_INVALID_ARG ? "file is not an ESP32 app image" : esp_err_to_name(err));
    }

    err = esp_ota_end(handle);               /* validates checksum / image header */
    if (err != ESP_OK) {
        s_in_progress = false;
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        command_manager_post_simple(CMD_RESUME, 0);
        return send_result(req, false, "400 Bad Request",
                           err == ESP_ERR_OTA_VALIDATE_FAILED ? "image validation failed" : esp_err_to_name(err));
    }
    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        s_in_progress = false;
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        command_manager_post_simple(CMD_RESUME, 0);
        return send_result(req, false, "500 Internal Server Error", esp_err_to_name(err));
    }

    ESP_LOGW(TAG, "update written to %s, rebooting in %d ms", target->label, OTA_RESTART_DELAY_MS);
    esp_err_t resp = send_result(req, true, "200 OK", "update written, device is restarting");

    if (s_restart_timer == NULL) {
        esp_timer_create_args_t targs = {
            .callback = restart_cb,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "ota_restart",
            .skip_unhandled_events = true,
        };
        esp_timer_create(&targs, &s_restart_timer);
    }
    if (s_restart_timer != NULL) {
        esp_timer_start_once(s_restart_timer, (uint64_t)OTA_RESTART_DELAY_MS * 1000);
    } else {
        vTaskDelay(pdMS_TO_TICKS(OTA_RESTART_DELAY_MS));
        esp_restart();
    }
    return resp;
}

void ota_update_mark_valid_if_pending(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (running == NULL || esp_ota_get_state_partition(running, &state) != ESP_OK) {
        return;
    }
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGW(TAG, "new firmware on %s marked valid: %s", running->label, esp_err_to_name(err));
    }
}
