/*
 * command_manager.h - single point of control for playback state.
 *
 * All commands (web, encoder, Wi-Fi events) go through one FreeRTOS queue
 * and are executed sequentially by the command task. STOP and station
 * changes are queued at the front; volume updates are coalesced so a fast
 * encoder spin never floods the queue.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "app_events.h"

esp_err_t command_manager_init(void);

/* Non-blocking post from a task. ESP_ERR_NO_MEM if the queue is full. */
esp_err_t command_manager_post(const app_cmd_t *cmd);
esp_err_t command_manager_post_simple(app_cmd_type_t type, int32_t arg);

/* ISR-safe post (uses xQueueSendFromISR / xQueueSendToFrontFromISR). */
BaseType_t command_manager_post_from_isr(const app_cmd_t *cmd, BaseType_t *hp_task_woken);

uint8_t  command_manager_get_volume(void);
int      command_manager_get_station(void);
uint32_t command_manager_get_dropped_count(void);
