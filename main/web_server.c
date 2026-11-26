/*
 * web_server.c - esp_http_server: static dashboard + JSON control API.
 *
 * Handlers never touch the audio pipeline. They validate the request,
 * post a command to the command manager with zero wait and reply at once.
 * The HTTP server task runs below the stream task so a burst of requests
 * can never starve the audio path on this single core.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "cJSON.h"

#include "app_config.h"
#include "app_status.h"
#include "station_list.h"
#include "stream_manager.h"
#include "audio_player.h"
#include "wifi_manager.h"
#include "command_manager.h"
#include "ota_update.h"
#include "web_server.h"

static const char *TAG = "web";

#define BODY_MAX    512

/* Embedded with EMBED_TXTFILES (null terminated, hence the -1 on length). */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");
extern const uint8_t style_css_start[]  asm("_binary_style_css_start");
extern const uint8_t style_css_end[]    asm("_binary_style_css_end");
extern const uint8_t app_js_start[]     asm("_binary_app_js_start");
extern const uint8_t app_js_end[]       asm("_binary_app_js_end");

static httpd_handle_t s_server = NULL;

/* ---- helpers ------------------------------------------------------------ */
static esp_err_t send_static(httpd_req_t *req, const char *type, const uint8_t *start, const uint8_t *end)
{
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=120");
    return httpd_resp_send(req, (const char *)start, (ssize_t)(end - start - 1));
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

static esp_err_t send_result(httpd_req_t *req, bool ok, const char *status, const char *cmd, const char *error)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json alloc");
    }
    cJSON_AddBoolToObject(root, "ok", ok);
    if (cmd) {
        cJSON_AddStringToObject(root, "cmd", cmd);
    }
    if (error) {
        cJSON_AddStringToObject(root, "error", error);
    }
    return send_json(req, status, root);
}

/* Read a small JSON body. Returns NULL (and sends an error) on failure.
 * A missing body (content_len == 0) returns an empty object. */
static cJSON *read_json_body(httpd_req_t *req)
{
    char buf[BODY_MAX];
    if (req->content_len >= sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "body too large");
        return NULL;
    }
    size_t got = 0;
    int retries = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && retries++ < 2) {
            continue;
        }
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "body receive failed");
            return NULL;
        }
        got += (size_t)r;
    }
    buf[got] = '\0';
    cJSON *root = (got == 0) ? cJSON_CreateObject() : cJSON_Parse(buf);
    if (root == NULL) {
        send_result(req, false, "400 Bad Request", NULL, "invalid json");
        return NULL;
    }
    return root;
}

static esp_err_t post_and_reply(httpd_req_t *req, app_cmd_t *cmd, const char *name)
{
    esp_err_t err = command_manager_post(cmd);
    if (err == ESP_ERR_NO_MEM) {
        return send_result(req, false, "503 Service Unavailable", name, "command queue full");
    }
    if (err != ESP_OK) {
        return send_result(req, false, "400 Bad Request", name, esp_err_to_name(err));
    }
    return send_result(req, true, "200 OK", name, NULL);
}

/* ---- static files ------------------------------------------------------- */
static esp_err_t index_get(httpd_req_t *req)
{
    return send_static(req, "text/html; charset=utf-8", index_html_start, index_html_end);
}

static esp_err_t css_get(httpd_req_t *req)
{
    return send_static(req, "text/css; charset=utf-8", style_css_start, style_css_end);
}

static esp_err_t js_get(httpd_req_t *req)
{
    return send_static(req, "application/javascript; charset=utf-8", app_js_start, app_js_end);
}

/* ---- GET /api/status ---------------------------------------------------- */
static esp_err_t status_get(httpd_req_t *req)
{
    app_status_t st;
    app_status_get(&st);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json alloc");
    }

    /* Minimal payload for the dashboard (polled every 1.5 s). The detailed
     * diagnostics live on GET /api/diag so the common path stays cheap. */
    cJSON *station = cJSON_AddObjectToObject(root, "station");
    cJSON_AddNumberToObject(station, "index", st.station_index);
    cJSON_AddStringToObject(station, "name", st.station_name);

    cJSON_AddStringToObject(root, "title", st.stream_title);
    cJSON_AddStringToObject(root, "state", playback_state_str(st.playback));
    cJSON_AddStringToObject(root, "error", st.error);
    cJSON_AddNumberToObject(root, "volume", st.volume);
    cJSON_AddBoolToObject(root, "mono", st.mono);

    cJSON *disp = cJSON_AddObjectToObject(root, "display");
    cJSON_AddStringToObject(disp, "type", display_type_str(st.display_type));
    cJSON_AddNumberToObject(disp, "rotation", st.display_rot);

    cJSON *bat = cJSON_AddObjectToObject(root, "battery");
    cJSON_AddNumberToObject(bat, "pct", st.battery_pct);
    cJSON_AddBoolToObject(bat, "simulated", true);

    char ip[16];
    char ssid[33];
    wifi_manager_get_ip(ip, sizeof(ip));
    wifi_manager_get_ssid(ssid, sizeof(ssid));
    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddBoolToObject(wifi, "connected", wifi_manager_is_connected());
    cJSON_AddBoolToObject(wifi, "ap_active", wifi_manager_is_ap_active());
    cJSON_AddStringToObject(wifi, "ip", ip);
    cJSON_AddStringToObject(wifi, "ssid", ssid);
    return send_json(req, "200 OK", root);
}

/* ---- GET /api/diag: technical counters (not polled by the dashboard) ---- */
static esp_err_t diag_get(httpd_req_t *req)
{
    app_status_t st;
    app_status_get(&st);
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json alloc");
    }
    cJSON_AddNumberToObject(root, "sample_rate", st.sample_rate);
    cJSON_AddNumberToObject(root, "channels", st.channels);
    cJSON_AddNumberToObject(root, "bitrate_kbps", st.bitrate_kbps);
    cJSON_AddBoolToObject(root, "xsmt_unmuted", st.unmuted);
    cJSON_AddNumberToObject(root, "level", st.level);
    cJSON_AddStringToObject(root, "knob_mode", st.encoder_mode == 0 ? "volume" : "station");
    size_t cap = stream_manager_capacity();
    size_t avail = stream_manager_bytes_available();
    cJSON_AddNumberToObject(root, "buffer_bytes", (double)avail);
    cJSON_AddNumberToObject(root, "buffer_capacity", (double)cap);
    cJSON_AddBoolToObject(root, "stream_connected", stream_manager_is_connected());
    cJSON_AddNumberToObject(root, "rssi", wifi_manager_get_rssi());
    cJSON_AddStringToObject(root, "wifi_state", wifi_manager_get_state_str());
    cJSON_AddNumberToObject(root, "heap_free", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "heap_min", (double)esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(root, "underruns", st.underruns);
    cJSON_AddNumberToObject(root, "dma_underruns", audio_player_get_dma_underruns());
    cJSON_AddNumberToObject(root, "reconnects", st.reconnects);
    cJSON_AddNumberToObject(root, "decoder_errors", st.decoder_errors);
    cJSON_AddNumberToObject(root, "dropped_cmds", command_manager_get_dropped_count());
    return send_json(req, "200 OK", root);
}

/* ---- GET /api/stations -------------------------------------------------- */
static esp_err_t stations_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json alloc");
    }
    cJSON *arr = cJSON_AddArrayToObject(root, "stations");
    for (size_t i = 0; i < station_list_count(); i++) {
        const station_t *s = station_list_get(i);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "index", (double)i);
        cJSON_AddStringToObject(o, "name", s->name);
        cJSON_AddStringToObject(o, "genre", s->genre);
        cJSON_AddStringToObject(o, "url", s->url);
        cJSON_AddStringToObject(o, "note", s->note);
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddNumberToObject(root, "current", command_manager_get_station());
    return send_json(req, "200 OK", root);
}

/* ---- POST /api/play|stop|next|previous ---------------------------------- */
static esp_err_t play_post(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (body == NULL) {
        return ESP_OK;
    }
    int idx = -1;
    cJSON *st = cJSON_GetObjectItemCaseSensitive(body, "station");
    if (cJSON_IsNumber(st)) {
        idx = st->valueint;
        if (idx < 0 || idx >= (int)station_list_count()) {
            cJSON_Delete(body);
            return send_result(req, false, "400 Bad Request", "play", "station out of range");
        }
    }
    cJSON_Delete(body);
    app_cmd_t cmd = { .type = CMD_PLAY_STATION, .arg = idx };
    return post_and_reply(req, &cmd, "play");
}

static esp_err_t simple_post(httpd_req_t *req, app_cmd_type_t type, const char *name)
{
    /* Drain any body so the connection stays in sync, then ignore it. */
    if (req->content_len > 0) {
        cJSON *body = read_json_body(req);
        if (body == NULL) {
            return ESP_OK;
        }
        cJSON_Delete(body);
    }
    app_cmd_t cmd = { .type = type, .arg = 0 };
    return post_and_reply(req, &cmd, name);
}

static esp_err_t stop_post(httpd_req_t *req)      { return simple_post(req, CMD_STOP, "stop"); }
static esp_err_t next_post(httpd_req_t *req)      { return simple_post(req, CMD_NEXT_STATION, "next"); }
static esp_err_t previous_post(httpd_req_t *req)  { return simple_post(req, CMD_PREVIOUS_STATION, "previous"); }
static esp_err_t resume_post(httpd_req_t *req)    { return simple_post(req, CMD_RESUME, "resume"); }

/* ---- POST /api/volume {"volume": 0..100} -------------------------------- */
static esp_err_t volume_post(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (body == NULL) {
        return ESP_OK;
    }
    cJSON *v = cJSON_GetObjectItemCaseSensitive(body, "volume");
    if (!cJSON_IsNumber(v) || v->valueint < 0 || v->valueint > 100) {
        cJSON_Delete(body);
        return send_result(req, false, "400 Bad Request", "volume", "volume must be 0..100");
    }
    app_cmd_t cmd = { .type = CMD_SET_VOLUME, .arg = v->valueint };
    cJSON_Delete(body);
    return post_and_reply(req, &cmd, "volume");
}

/* ---- GET /api/scan ------------------------------------------------------ */
static const char *authmode_str(wifi_auth_mode_t m)
{
    switch (m) {
    case WIFI_AUTH_OPEN:            return "open";
    case WIFI_AUTH_WEP:             return "wep";
    case WIFI_AUTH_WPA_PSK:         return "wpa";
    case WIFI_AUTH_WPA2_PSK:        return "wpa2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "wpa/wpa2";
    case WIFI_AUTH_WPA3_PSK:        return "wpa3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "wpa2/wpa3";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "wpa2-ent";
    default:                        return "other";
    }
}

static esp_err_t scan_get(httpd_req_t *req)
{
    static wifi_ap_record_t records[APP_WIFI_SCAN_MAX_RECORDS];   /* httpd is single threaded */
    uint16_t count = APP_WIFI_SCAN_MAX_RECORDS;
    esp_err_t err = wifi_manager_scan(records, &count);
    if (err != ESP_OK) {
        const char *why = (err == ESP_ERR_WIFI_STATE) ? "busy connecting, retry in a few seconds"
                        : (err == ESP_ERR_TIMEOUT)    ? "scan already running"
                        : esp_err_to_name(err);
        return send_result(req, false, "503 Service Unavailable", "scan", why);
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json alloc");
    }
    cJSON *arr = cJSON_AddArrayToObject(root, "networks");
    for (uint16_t i = 0; i < count; i++) {
        if (records[i].ssid[0] == '\0') {
            continue;
        }
        /* Keep only the strongest BSSID per SSID (the driver sorts by RSSI). */
        bool dup = false;
        for (uint16_t j = 0; j < i; j++) {
            if (strncmp((const char *)records[i].ssid, (const char *)records[j].ssid, sizeof(records[i].ssid)) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        cJSON *o = cJSON_CreateObject();
        char ssid[33];
        memcpy(ssid, records[i].ssid, 32);
        ssid[32] = '\0';
        cJSON_AddStringToObject(o, "ssid", ssid);
        cJSON_AddNumberToObject(o, "rssi", records[i].rssi);
        cJSON_AddNumberToObject(o, "channel", records[i].primary);
        cJSON_AddStringToObject(o, "auth", authmode_str(records[i].authmode));
        cJSON_AddBoolToObject(o, "secure", records[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddNumberToObject(root, "count", (double)count);
    return send_json(req, "200 OK", root);
}

/* ---- captive portal: unknown URI -> redirect to the dashboard ----------- */
static esp_err_t not_found_handler(httpd_req_t *req, httpd_err_code_t error)
{
    (void)error;
    if (wifi_manager_is_ap_active()) {
        /* Android generate_204, Apple hotspot-detect, Windows connecttest all land here. */
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", APP_CAPTIVE_PORTAL_URL);
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "not found", HTTPD_RESP_USE_STRLEN);
}

/* ---- POST /api/mono {"mono": true|false} -------------------------------- */
static esp_err_t mono_post(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (body == NULL) {
        return ESP_OK;
    }
    cJSON *m = cJSON_GetObjectItemCaseSensitive(body, "mono");
    if (!cJSON_IsBool(m)) {
        cJSON_Delete(body);
        return send_result(req, false, "400 Bad Request", "mono", "mono must be true or false");
    }
    app_cmd_t cmd = { .type = CMD_SET_MONO, .arg = cJSON_IsTrue(m) ? 1 : 0 };
    cJSON_Delete(body);
    return post_and_reply(req, &cmd, "mono");
}

/* ---- POST /api/display {"type":"epd_154"|"st7789_240", "rotation":0..270} - */
static esp_err_t display_post(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (body == NULL) {
        return ESP_OK;
    }
    uint16_t type_arg = 0xFFFF;
    uint16_t rot_arg = 0xFFFF;

    cJSON *t = cJSON_GetObjectItemCaseSensitive(body, "type");
    if (t != NULL) {
        if (!cJSON_IsString(t) || t->valuestring == NULL) {
            cJSON_Delete(body);
            return send_result(req, false, "400 Bad Request", "display", "invalid display type");
        }
        if (strcmp(t->valuestring, "epd_154") == 0) {
            type_arg = DISPLAY_EPD_154;
        } else if (strcmp(t->valuestring, "st7789_240") == 0) {
            type_arg = DISPLAY_ST7789_240;
        } else {
            cJSON_Delete(body);
            return send_result(req, false, "400 Bad Request", "display", "unknown display type");
        }
    }

    cJSON *r = cJSON_GetObjectItemCaseSensitive(body, "rotation");
    if (r != NULL) {
        if (!cJSON_IsNumber(r)) {
            cJSON_Delete(body);
            return send_result(req, false, "400 Bad Request", "display", "rotation must be 0, 90, 180 or 270");
        }
        int rv = r->valueint;
        if (rv != 0 && rv != 90 && rv != 180 && rv != 270) {
            cJSON_Delete(body);
            return send_result(req, false, "400 Bad Request", "display", "rotation must be 0, 90, 180 or 270");
        }
        rot_arg = (uint16_t)rv;
    }

    cJSON_Delete(body);
    if (type_arg == 0xFFFF && rot_arg == 0xFFFF) {
        return send_result(req, false, "400 Bad Request", "display", "type or rotation required");
    }
    app_cmd_t cmd = {
        .type = CMD_SET_DISPLAY,
        .arg = ((int32_t)type_arg << 16) | (int32_t)rot_arg,
    };
    return post_and_reply(req, &cmd, "display");
}

/* ---- GET/POST /api/wifi ------------------------------------------------- */
static esp_err_t wifi_get(httpd_req_t *req)
{
    char ip[16];
    char ssid[33];
    wifi_manager_get_ip(ip, sizeof(ip));
    wifi_manager_get_ssid(ssid, sizeof(ssid));
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json alloc");
    }
    cJSON_AddBoolToObject(root, "connected", wifi_manager_is_connected());
    cJSON_AddBoolToObject(root, "ap_active", wifi_manager_is_ap_active());
    cJSON_AddBoolToObject(root, "has_credentials", wifi_manager_has_credentials());
    cJSON_AddStringToObject(root, "ssid", ssid);        /* password is never returned */
    cJSON_AddStringToObject(root, "ip", ip);
    cJSON_AddNumberToObject(root, "rssi", wifi_manager_get_rssi());
    cJSON_AddStringToObject(root, "state", wifi_manager_get_state_str());
    cJSON_AddStringToObject(root, "ap_ssid", APP_WIFI_AP_SSID);
    return send_json(req, "200 OK", root);
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (body == NULL) {
        return ESP_OK;
    }
    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(body, "ssid");
    cJSON *pass = cJSON_GetObjectItemCaseSensitive(body, "password");
    if (!cJSON_IsString(ssid) || ssid->valuestring == NULL) {
        cJSON_Delete(body);
        return send_result(req, false, "400 Bad Request", "wifi", "ssid required");
    }
    const char *pw = (cJSON_IsString(pass) && pass->valuestring) ? pass->valuestring : "";
    size_t slen = strlen(ssid->valuestring);
    size_t plen = strlen(pw);
    if (slen == 0 || slen > 32) {
        cJSON_Delete(body);
        return send_result(req, false, "400 Bad Request", "wifi", "ssid must be 1..32 chars");
    }
    if (plen > 63 || (plen > 0 && plen < 8)) {
        cJSON_Delete(body);
        return send_result(req, false, "400 Bad Request", "wifi", "password must be empty or 8..63 chars");
    }
    app_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_WIFI_CONFIG;
    strncpy(cmd.ssid, ssid->valuestring, sizeof(cmd.ssid) - 1);
    strncpy(cmd.password, pw, sizeof(cmd.password) - 1);
    cJSON_Delete(body);
    ESP_LOGI(TAG, "wifi config request for SSID '%s'", cmd.ssid);
    esp_err_t err = post_and_reply(req, &cmd, "wifi");
    memset(cmd.password, 0, sizeof(cmd.password));
    return err;
}

/* ---- server ------------------------------------------------------------- */
esp_err_t web_server_start(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.task_priority = APP_HTTPD_PRIO;
    cfg.stack_size = APP_HTTPD_STACK;
    cfg.max_uri_handlers = 24;
    cfg.recv_wait_timeout = 10;         /* OTA uploads over slow Wi-Fi */
    cfg.max_open_sockets = APP_HTTPD_MAX_SOCKETS;
    cfg.lru_purge_enable = true;
    cfg.send_wait_timeout = 5;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t routes[] = {
        { .uri = "/",              .method = HTTP_GET,  .handler = index_get,     .user_ctx = NULL },
        { .uri = "/index.html",    .method = HTTP_GET,  .handler = index_get,     .user_ctx = NULL },
        { .uri = "/style.css",     .method = HTTP_GET,  .handler = css_get,       .user_ctx = NULL },
        { .uri = "/app.js",        .method = HTTP_GET,  .handler = js_get,        .user_ctx = NULL },
        { .uri = "/api/status",    .method = HTTP_GET,  .handler = status_get,    .user_ctx = NULL },
        { .uri = "/api/diag",      .method = HTTP_GET,  .handler = diag_get,      .user_ctx = NULL },
        { .uri = "/api/stations",  .method = HTTP_GET,  .handler = stations_get,  .user_ctx = NULL },
        { .uri = "/api/play",      .method = HTTP_POST, .handler = play_post,     .user_ctx = NULL },
        { .uri = "/api/stop",      .method = HTTP_POST, .handler = stop_post,     .user_ctx = NULL },
        { .uri = "/api/resume",    .method = HTTP_POST, .handler = resume_post,   .user_ctx = NULL },
        { .uri = "/api/next",      .method = HTTP_POST, .handler = next_post,     .user_ctx = NULL },
        { .uri = "/api/previous",  .method = HTTP_POST, .handler = previous_post, .user_ctx = NULL },
        { .uri = "/api/volume",    .method = HTTP_POST, .handler = volume_post,   .user_ctx = NULL },
        { .uri = "/api/wifi",      .method = HTTP_GET,  .handler = wifi_get,      .user_ctx = NULL },
        { .uri = "/api/wifi",      .method = HTTP_POST, .handler = wifi_post,     .user_ctx = NULL },
        { .uri = "/api/scan",      .method = HTTP_GET,  .handler = scan_get,      .user_ctx = NULL },
        { .uri = "/api/mono",      .method = HTTP_POST, .handler = mono_post,     .user_ctx = NULL },
        { .uri = "/api/display",   .method = HTTP_POST, .handler = display_post,  .user_ctx = NULL },
        { .uri = "/api/ota",       .method = HTTP_GET,  .handler = ota_update_handle_info,   .user_ctx = NULL },
        { .uri = "/api/ota",       .method = HTTP_POST, .handler = ota_update_handle_upload, .user_ctx = NULL },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        err = httpd_register_uri_handler(s_server, &routes[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %s: %s", routes[i].uri, esp_err_to_name(err));
            httpd_stop(s_server);
            s_server = NULL;
            return err;
        }
    }
    err = httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, not_found_handler);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "404 handler: %s (captive redirect disabled)", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "dashboard on port %d (%u routes, captive redirect on)", cfg.server_port,
             (unsigned)(sizeof(routes) / sizeof(routes[0])));
    return ESP_OK;
}
