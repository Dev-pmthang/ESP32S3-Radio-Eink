/*
 * wifi_manager.h - STA with NVS credentials, APSTA fallback, event driven.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_wifi_types.h"

/* Called from the Wi-Fi event handler context when STA gets / loses IP. */
typedef void (*wifi_manager_cb_t)(bool connected);

esp_err_t wifi_manager_init(wifi_manager_cb_t cb);
esp_err_t wifi_manager_start(void);

/* Persist new credentials to NVS (namespace wifi_cfg) and reconnect. */
esp_err_t wifi_manager_set_credentials(const char *ssid, const char *password);

/* Erase the stored credentials (namespace wifi_cfg). Takes effect at next boot. */
esp_err_t wifi_manager_clear_credentials(void);

/* Blocking scan (a few seconds). *count holds the array capacity on entry and
 * the number of records written on return. Needs a mode that includes STA. */
esp_err_t wifi_manager_scan(wifi_ap_record_t *records, uint16_t *count);

bool wifi_manager_has_credentials(void);
bool wifi_manager_is_connected(void);
bool wifi_manager_is_ap_active(void);
int  wifi_manager_get_rssi(void);                 /* 0 when not connected */
void wifi_manager_get_ip(char *buf, size_t len);  /* "0.0.0.0" when down  */
void wifi_manager_get_ssid(char *buf, size_t len);
const char *wifi_manager_get_state_str(void);
