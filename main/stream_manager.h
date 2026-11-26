/*
 * stream_manager.h - HTTP(S) MP3 stream producer (http_stream_task).
 *
 * The task reads the radio stream in chunks with esp_http_client, strips
 * ICY metadata and pushes raw MP3 bytes into a 64 KB static FreeRTOS
 * stream buffer that the decoder task consumes.
 *
 * Lifecycle (driven by the command manager, in this order):
 *   stream_manager_stop()  -> task aborts the socket and goes idle
 *   audio_decoder_stop()   -> decoder goes idle
 *   stream_manager_reset_buffer()   (both idle, nobody blocked on the buffer)
 *   stream_manager_start(url)
 *   audio_decoder_start()
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

esp_err_t stream_manager_init(void);

/* Start streaming 'url' (copied internally, max APP_STREAM_URL_MAX-1 chars). */
esp_err_t stream_manager_start(const char *url);

/* Request abort and wait until the task is idle. ESP_ERR_TIMEOUT if it is
 * still inside a blocking socket call after timeout_ms. */
esp_err_t stream_manager_stop(uint32_t timeout_ms);

/* Discard buffered data. Only valid while producer and consumer are idle. */
esp_err_t stream_manager_reset_buffer(void);

/* Consumer side (decoder task only). */
size_t stream_manager_read(uint8_t *dst, size_t max_len, uint32_t timeout_ms);
esp_err_t stream_manager_set_trigger_level(size_t bytes);

size_t stream_manager_bytes_available(void);
size_t stream_manager_capacity(void);

/* true while an HTTP connection is open and delivering data. */
bool stream_manager_is_connected(void);
/* true between start() and stop(). */
bool stream_manager_is_active(void);
