/*
 * ota_update.h - firmware update over the web dashboard.
 *
 *   GET  /api/ota   -> running version / partition info (JSON)
 *   POST /api/ota   -> raw application image (.bin) in the request body
 *
 * Flow: stop playback (frees CPU and RAM, no audio glitches during flash
 * erase), esp_ota_begin/write/end into the inactive slot, set it as boot
 * partition, reply, restart 1.5 s later. The new image boots as
 * "pending verify"; ota_update_mark_valid_if_pending() confirms it once the
 * application has been running normally, else the bootloader rolls back.
 */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t ota_update_handle_info(httpd_req_t *req);
esp_err_t ota_update_handle_upload(httpd_req_t *req);

/* Call once the application is known to be healthy (e.g. 10 s after boot). */
void ota_update_mark_valid_if_pending(void);
