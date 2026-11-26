/*
 * wifi_manager.c - STA + APSTA fallback, credentials in NVS (wifi_cfg).
 *
 * Flow:
 *   boot --credentials--> STA connect (max APP_WIFI_MAX_RETRY)
 *                      \--> fail: APSTA, SoftAP "ESP32-C5-Radio" @192.168.4.1,
 *                           STA retried every APP_WIFI_STA_RETRY_PERIOD_MS
 *   boot --no creds-----> AP only, waiting for POST /api/wifi
 *
 * Everything is event driven (esp_event + esp_timer); nothing blocks.
 * Passwords are never logged.
 */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_idf_version.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "dhcpserver/dhcpserver.h"

#include "app_config.h"
#include "captive_dns.h"
#include "wifi_manager.h"

static const char *TAG = "wifi";

typedef enum {
    WM_IDLE = 0,
    WM_CONNECTING,
    WM_CONNECTED,
    WM_AP_FALLBACK,     /* APSTA: AP up, STA retrying in background */
    WM_AP_ONLY,         /* no credentials stored */
} wm_state_t;

static esp_netif_t        *s_sta_netif = NULL;
static esp_netif_t        *s_ap_netif = NULL;
static wifi_manager_cb_t   s_cb = NULL;
static esp_timer_handle_t  s_retry_timer = NULL;

static char                s_ssid[33];
static char                s_pass[65];
static bool                s_has_creds = false;
static int                 s_retry = 0;
static volatile bool       s_connected = false;
static volatile bool       s_ap_active = false;
static volatile wm_state_t s_state = WM_IDLE;
static char                s_ip[16] = "0.0.0.0";
static SemaphoreHandle_t   s_scan_lock = NULL;
static StaticSemaphore_t   s_scan_lock_buf;

/* ---- captive portal plumbing ------------------------------------------- */
/* Make the SoftAP DHCP server hand out 192.168.4.1 as DNS server and
 * advertise the portal URL (DHCP option 114). Must run while dhcps is
 * stopped, i.e. right after the AP netif is created. */
static void configure_captive_dhcp(void)
{
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_ap_netif, &ip_info) != ESP_OK) {
        return;
    }
    esp_err_t err = esp_netif_dhcps_stop(s_ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "dhcps stop: %s", esp_err_to_name(err));
    }
    dhcps_offer_t offer = OFFER_DNS;
    err = esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof(offer));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "dhcps OFFER_DNS: %s", esp_err_to_name(err));
    }
    esp_netif_dns_info_t dns;
    memset(&dns, 0, sizeof(dns));
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ip_info.ip.addr;
    err = esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AP dns info: %s", esp_err_to_name(err));
    }
    static const char portal_url[] = APP_CAPTIVE_PORTAL_URL;
    err = esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                                 (void *)portal_url, sizeof(portal_url) - 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "dhcps captive URI: %s", esp_err_to_name(err));
    }
    /* dhcps is started by the default esp_netif handler on WIFI_EVENT_AP_START. */
}

/* ---- NVS ---------------------------------------------------------------- */
/* Fall back to the compile-time credentials when NVS holds none. */
static void load_default_credentials(void)
{
    if (APP_WIFI_STA_DEFAULT_SSID[0] == '\0') {
        ESP_LOGI(TAG, "no stored credentials");
        return;
    }
    strncpy(s_ssid, APP_WIFI_STA_DEFAULT_SSID, sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';
    strncpy(s_pass, APP_WIFI_STA_DEFAULT_PASSWORD, sizeof(s_pass) - 1);
    s_pass[sizeof(s_pass) - 1] = '\0';
    s_has_creds = true;
    ESP_LOGI(TAG, "no stored credentials, using compile-time default SSID '%s'", s_ssid);
}

static void load_credentials(void)
{
    nvs_handle_t h;
    s_has_creds = false;
    if (nvs_open(APP_WIFI_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        load_default_credentials();
        return;
    }
    size_t len = sizeof(s_ssid);
    esp_err_t err = nvs_get_str(h, "ssid", s_ssid, &len);
    if (err == ESP_OK && s_ssid[0] != '\0') {
        len = sizeof(s_pass);
        if (nvs_get_str(h, "pass", s_pass, &len) != ESP_OK) {
            s_pass[0] = '\0';
        }
        s_has_creds = true;
        ESP_LOGI(TAG, "credentials loaded for SSID '%s'", s_ssid);
    }
    nvs_close(h);
    if (!s_has_creds) {
        load_default_credentials();
    }
}

static esp_err_t save_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(APP_WIFI_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, "ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "pass", pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* ---- config builders ---------------------------------------------------- */
static void apply_sta_config(void)
{
    wifi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    /* ssid[32] / password[64] are NOT required to be NUL terminated by the
     * driver; copy the exact length so GCC's stringop-truncation stays quiet. */
    memcpy(cfg.sta.ssid, s_ssid, strnlen(s_ssid, sizeof(cfg.sta.ssid)));
    memcpy(cfg.sta.password, s_pass, strnlen(s_pass, sizeof(cfg.sta.password)));
    /* FAST_SCAN connects to the first matching AP (measured ~2 s) instead of
     * sweeping every 2.4 + 5 GHz channel first (measured ~12 s on the C5). */
    cfg.sta.scan_method = WIFI_FAST_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    cfg.sta.threshold.rssi = -127;
    cfg.sta.threshold.authmode = (s_pass[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    cfg.sta.failure_retry_cnt = 2;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set STA config: %s", esp_err_to_name(err));
    }
}

static void apply_ap_config(void)
{
    wifi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    memcpy(cfg.ap.ssid, APP_WIFI_AP_SSID, strnlen(APP_WIFI_AP_SSID, sizeof(cfg.ap.ssid)));
    cfg.ap.ssid_len = (uint8_t)strnlen(APP_WIFI_AP_SSID, sizeof(cfg.ap.ssid));
    cfg.ap.channel = APP_WIFI_AP_CHANNEL;
    cfg.ap.max_connection = APP_WIFI_AP_MAX_CONN;
    if (strlen(APP_WIFI_AP_PASSWORD) >= 8) {
        memcpy(cfg.ap.password, APP_WIFI_AP_PASSWORD, strnlen(APP_WIFI_AP_PASSWORD, sizeof(cfg.ap.password)));
        cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        cfg.ap.authmode = WIFI_AUTH_OPEN;
    }
    cfg.ap.pmf_cfg.capable = true;
    cfg.ap.pmf_cfg.required = false;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set AP config: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "SoftAP config applied: SSID '%s', channel %d, %s", APP_WIFI_AP_SSID,
                 APP_WIFI_AP_CHANNEL, cfg.ap.authmode == WIFI_AUTH_OPEN ? "open" : "WPA2");
    }
}

static void start_ap_fallback(void)
{
    if (s_ap_active) {
        return;
    }
    ESP_LOGW(TAG, "STA failed %d times -> APSTA fallback, SoftAP '%s'", APP_WIFI_MAX_RETRY, APP_WIFI_AP_SSID);
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_mode APSTA: %s", esp_err_to_name(err));
        return;
    }
    apply_ap_config();
    s_ap_active = true;
    s_state = WM_AP_FALLBACK;
    if (s_retry_timer != NULL) {
        esp_timer_stop(s_retry_timer);
        esp_timer_start_periodic(s_retry_timer, (uint64_t)APP_WIFI_STA_RETRY_PERIOD_MS * 1000);
    }
}

static void retry_timer_cb(void *arg)
{
    (void)arg;
    if (!s_connected && s_has_creds) {
        ESP_LOGI(TAG, "background STA retry");
        s_state = WM_CONNECTING;
        esp_wifi_connect();
    }
}

/* ---- events ------------------------------------------------------------- */
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA started");
            break;
        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "associated, waiting for IP");
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
            bool was_connected = s_connected;
            s_connected = false;
            strcpy(s_ip, "0.0.0.0");
            ESP_LOGW(TAG, "disconnected (reason %d)", ev ? ev->reason : -1);
            if (was_connected && s_cb) {
                s_cb(false);
            }
            if (!s_has_creds) {
                break;
            }
            if (s_retry < APP_WIFI_MAX_RETRY) {
                s_retry++;
                s_state = WM_CONNECTING;
                ESP_LOGI(TAG, "reconnect attempt %d/%d", s_retry, APP_WIFI_MAX_RETRY);
                esp_wifi_connect();
            } else {
                start_ap_fallback();
            }
            break;
        }
        case WIFI_EVENT_AP_START: {
            ESP_LOGI(TAG, "SoftAP up: SSID '%s' (192.168.4.1)", APP_WIFI_AP_SSID);
            esp_netif_ip_info_t ip_info;
            if (esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK) {
                if (captive_dns_start(ip_info.ip.addr) != ESP_OK) {
                    ESP_LOGW(TAG, "captive DNS not started");
                }
            }
            break;
        }
        case WIFI_EVENT_AP_STOP:
            ESP_LOGI(TAG, "SoftAP stopped");
            captive_dns_stop();
            break;
        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(TAG, "client joined SoftAP");
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            ESP_LOGI(TAG, "client left SoftAP");
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry = 0;
        s_connected = true;
        s_state = WM_CONNECTED;
        ESP_LOGI(TAG, "got IP %s", s_ip);
        if (s_retry_timer != NULL) {
            esp_timer_stop(s_retry_timer);
        }
#if APP_WIFI_AP_STOP_WHEN_CONNECTED
        if (s_ap_active) {
            ESP_LOGI(TAG, "STA connected, stopping fallback SoftAP");
            esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
            if (err == ESP_OK) {
                s_ap_active = false;
            }
        }
#endif
        if (s_cb) {
            s_cb(true);
        }
    }
}

/* ---- public API --------------------------------------------------------- */
esp_err_t wifi_manager_init(wifi_manager_cb_t cb)
{
    s_cb = cb;
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_sta_netif == NULL || s_ap_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }
    configure_captive_dhcp();
    s_scan_lock = xSemaphoreCreateMutexStatic(&s_scan_lock_buf);

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
        return err;
    }
    /* Credentials live in our own NVS namespace; keep the driver's copy in RAM. */
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }

    esp_timer_create_args_t targs = {
        .callback = retry_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_retry",
        .skip_unhandled_events = true,
    };
    err = esp_timer_create(&targs, &s_retry_timer);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "retry timer: %s", esp_err_to_name(err));
        s_retry_timer = NULL;
    }

    load_credentials();
    return ESP_OK;
}

esp_err_t wifi_manager_start(void)
{
    esp_err_t err;
    /* Measured on ESP32-C5 / IDF v5.5.6: the driver comes up in band mode
     * 5G_ONLY, and esp_wifi_set_band_mode() is only accepted after
     * esp_wifi_start(). A 2.4 GHz SoftAP config (channel 1) set before start
     * is therefore rejected with ESP_ERR_INVALID_ARG. Order used here:
     *   set_mode -> start -> set_band_mode(AUTO) -> set AP config -> connect */
    if (s_has_creds) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) {
            return err;
        }
        apply_sta_config();                 /* STA config carries no channel */
        s_state = WM_CONNECTING;
    } else {
        /* APSTA so the portal can scan for networks while the AP is up. */
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) {
            return err;
        }
        s_state = WM_AP_ONLY;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_set_ps(WIFI_PS_NONE);     /* streaming: no modem sleep latency */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_ps: %s", esp_err_to_name(err));
    }

#if CONFIG_SOC_WIFI_SUPPORT_5G && (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0))
    /* ESP32-C5: let the driver pick 2.4 GHz or 5 GHz (one band at a time). */
    err = esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_band_mode(AUTO): %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "band mode: 2.4 GHz + 5 GHz (auto)");
    }
#else
    ESP_LOGI(TAG, "band mode API not available in this build: 2.4 GHz only");
#endif

    if (!s_has_creds) {
        /* Now that 2.4 GHz is allowed, replace the driver's default AP
         * settings with ours (SSID ESP32-C5-Radio, channel 1). */
        apply_ap_config();
        s_ap_active = true;
    } else {
        err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
        }
    }
    return ESP_OK;
}

esp_err_t wifi_manager_set_credentials(const char *ssid, const char *password)
{
    if (ssid == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t slen = strlen(ssid);
    size_t plen = strlen(password);
    if (slen == 0 || slen > 32 || plen > 63 || (plen > 0 && plen < 8)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = save_credentials(ssid, password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save credentials: %s", esp_err_to_name(err));
        return err;
    }
    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';
    strncpy(s_pass, password, sizeof(s_pass) - 1);
    s_pass[sizeof(s_pass) - 1] = '\0';
    s_has_creds = true;
    s_retry = 0;
    ESP_LOGI(TAG, "new credentials for SSID '%s' saved, reconnecting", s_ssid);

    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_AP) {
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (s_ap_active) {
        s_state = WM_AP_FALLBACK;   /* AP stays up until STA gets an IP */
    }
    apply_sta_config();
    esp_wifi_disconnect();      /* ESP_ERR_WIFI_NOT_STARTED/NOT_CONNECT are harmless here */
    s_connected = false;
    s_state = (s_state == WM_AP_FALLBACK) ? WM_AP_FALLBACK : WM_CONNECTING;
    return esp_wifi_connect();
}

esp_err_t wifi_manager_clear_credentials(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(APP_WIFI_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        memset(s_ssid, 0, sizeof(s_ssid));
        memset(s_pass, 0, sizeof(s_pass));
        s_has_creds = false;
        ESP_LOGW(TAG, "stored Wi-Fi credentials erased");
    }
    return err;
}

esp_err_t wifi_manager_scan(wifi_ap_record_t *records, uint16_t *count)
{
    if (records == NULL || count == NULL || *count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    if (mode != WIFI_MODE_STA && mode != WIFI_MODE_APSTA) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_scan_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;         /* another scan is in progress */
    }
    wifi_scan_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.show_hidden = false;
    cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    cfg.scan_time.active.min = APP_WIFI_SCAN_TIME_MIN_MS;
    cfg.scan_time.active.max = APP_WIFI_SCAN_TIME_MAX_MS;
    esp_err_t err = esp_wifi_scan_start(&cfg, true);     /* blocks for the scan duration */
    if (err == ESP_OK) {
        uint16_t found = 0;
        esp_wifi_scan_get_ap_num(&found);
        if (found < *count) {
            *count = found;
        }
        err = esp_wifi_scan_get_ap_records(count, records);  /* also frees the driver list */
        if (err != ESP_OK) {
            *count = 0;
            esp_wifi_clear_ap_list();
        }
    } else {
        *count = 0;
    }
    xSemaphoreGive(s_scan_lock);
    return err;
}

bool wifi_manager_has_credentials(void)
{
    return s_has_creds;
}

bool wifi_manager_is_connected(void)
{
    return s_connected;
}

bool wifi_manager_is_ap_active(void)
{
    return s_ap_active;
}

int wifi_manager_get_rssi(void)
{
    if (!s_connected) {
        return 0;
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    return ap.rssi;
}

void wifi_manager_get_ip(char *buf, size_t len)
{
    strncpy(buf, s_ip, len - 1);
    buf[len - 1] = '\0';
}

void wifi_manager_get_ssid(char *buf, size_t len)
{
    strncpy(buf, s_has_creds ? s_ssid : "", len - 1);
    buf[len - 1] = '\0';
}

const char *wifi_manager_get_state_str(void)
{
    switch (s_state) {
    case WM_IDLE:        return "idle";
    case WM_CONNECTING:  return "connecting";
    case WM_CONNECTED:   return "connected";
    case WM_AP_FALLBACK: return "ap_fallback";
    case WM_AP_ONLY:     return "ap_only";
    default:             return "unknown";
    }
}
