/*
 * stream_manager.c - http_stream_task: esp_http_client based MP3 producer.
 *
 * Blocking model (honest statement): esp_http_client is a synchronous API.
 * esp_http_client_open() blocks for DNS + TCP (+ TLS) up to timeout_ms per
 * step and esp_http_client_read() blocks up to timeout_ms when no data
 * arrives. The abort flag is checked after every call, so the worst case
 * reaction time to a station change is about APP_HTTP_TIMEOUT_MS while a
 * stream is stalled, and a few milliseconds while data is flowing.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/stream_buffer.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#include "app_config.h"
#include "app_status.h"
#include "stream_manager.h"

static const char *TAG = "stream";

#define EV_START    BIT0
#define EV_ABORT    BIT1
#define EV_IDLE     BIT2

typedef enum {
    ICY_AUDIO = 0,
    ICY_LEN,
    ICY_META,
} icy_state_t;

typedef struct {
    int         metaint;        /* 0 = stream has no in-band metadata     */
    icy_state_t state;
    int         bytes_to_meta;  /* audio bytes left before next meta block */
    int         meta_len;       /* size of the metadata block being read   */
    int         meta_got;       /* bytes of that block consumed so far     */
    char        meta[512];      /* first 511 bytes of the block            */
    char        icy_name[64];
    char        icy_genre[32];
    int         icy_br;
} icy_ctx_t;

/* ---- static allocations ------------------------------------------------- */
static uint8_t              s_sb_storage[APP_STREAM_BUFFER_SIZE + 1];
static StaticStreamBuffer_t s_sb_struct;
static StreamBufferHandle_t s_sb = NULL;

static StaticEventGroup_t   s_events_buf;
static EventGroupHandle_t   s_events = NULL;

static TaskHandle_t         s_task = NULL;
static char                 s_url[APP_STREAM_URL_MAX];
static uint8_t              s_rx[APP_HTTP_RX_CHUNK];
static icy_ctx_t            s_icy;

static volatile bool        s_active = false;
static volatile bool        s_connected = false;

/* ---- helpers ------------------------------------------------------------ */
static inline bool abort_requested(void)
{
    return (xEventGroupGetBits(s_events) & EV_ABORT) != 0;
}

/* Sleep up to 'ms' but wake immediately when abort is requested. */
static void wait_abortable(uint32_t ms)
{
    xEventGroupWaitBits(s_events, EV_ABORT, pdFALSE, pdFALSE, pdMS_TO_TICKS(ms));
}

static void copy_header_value(char *dst, size_t dst_size, const char *src)
{
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key != NULL && evt->header_value != NULL) {
        if (strcasecmp(evt->header_key, "icy-metaint") == 0) {
            s_icy.metaint = atoi(evt->header_value);
            if (s_icy.metaint < 0 || s_icy.metaint > (1 << 20)) {
                s_icy.metaint = 0;   /* implausible value: ignore metadata */
            }
        } else if (strcasecmp(evt->header_key, "icy-name") == 0) {
            copy_header_value(s_icy.icy_name, sizeof(s_icy.icy_name), evt->header_value);
        } else if (strcasecmp(evt->header_key, "icy-genre") == 0) {
            copy_header_value(s_icy.icy_genre, sizeof(s_icy.icy_genre), evt->header_value);
        } else if (strcasecmp(evt->header_key, "icy-br") == 0) {
            s_icy.icy_br = atoi(evt->header_value);
        } else if (strcasecmp(evt->header_key, "Content-Type") == 0) {
            if (strncasecmp(evt->header_value, "audio/mpeg", 10) != 0 &&
                strncasecmp(evt->header_value, "audio/mp3", 9) != 0 &&
                strncasecmp(evt->header_value, "application/octet-stream", 24) != 0) {
                ESP_LOGW(TAG, "unexpected Content-Type '%s' (pipeline decodes MP3 only)", evt->header_value);
            }
        }
    }
    return ESP_OK;
}

/* Push raw MP3 bytes into the stream buffer with back-pressure. */
static bool push_audio(const uint8_t *data, size_t len)
{
    while (len > 0) {
        if (abort_requested()) {
            return false;
        }
        size_t sent = xStreamBufferSend(s_sb, data, len, pdMS_TO_TICKS(100));
        data += sent;
        len -= sent;
    }
    return true;
}

/* Extract StreamTitle='...'; from an ICY metadata block. */
static void parse_icy_title(const char *meta)
{
    const char *key = "StreamTitle='";
    const char *p = strstr(meta, key);
    if (p == NULL) {
        return;
    }
    p += strlen(key);
    const char *end = strstr(p, "';");
    size_t n = end ? (size_t)(end - p) : strlen(p);
    static char last_title[128];
    char title[128];
    if (n >= sizeof(title)) {
        n = sizeof(title) - 1;
    }
    memcpy(title, p, n);
    title[n] = '\0';
    if (title[0] == '\0' || strcmp(title, last_title) == 0) {
        return;     /* empty (KEXP between tracks) or repeated (Classic FM every block) */
    }
    strcpy(last_title, title);
    app_status_set_stream_title(title);
    ESP_LOGI(TAG, "now playing: %s", title);
}

/* Split the received chunk into audio payload and ICY metadata. */
static bool process_chunk(const uint8_t *p, size_t n)
{
    if (s_icy.metaint <= 0) {
        return push_audio(p, n);
    }
    while (n > 0) {
        switch (s_icy.state) {
        case ICY_AUDIO: {
            size_t take = n < (size_t)s_icy.bytes_to_meta ? n : (size_t)s_icy.bytes_to_meta;
            if (!push_audio(p, take)) {
                return false;
            }
            p += take;
            n -= take;
            s_icy.bytes_to_meta -= (int)take;
            if (s_icy.bytes_to_meta == 0) {
                s_icy.state = ICY_LEN;
            }
            break;
        }
        case ICY_LEN:
            s_icy.meta_len = (int)(*p) * 16;
            s_icy.meta_got = 0;
            p++;
            n--;
            if (s_icy.meta_len == 0) {
                s_icy.state = ICY_AUDIO;
                s_icy.bytes_to_meta = s_icy.metaint;
            } else {
                s_icy.state = ICY_META;
            }
            break;
        case ICY_META: {
            int remaining = s_icy.meta_len - s_icy.meta_got;
            size_t take = n < (size_t)remaining ? n : (size_t)remaining;
            /* Keep the first part of the block, discard the rest. */
            for (size_t i = 0; i < take; i++) {
                int idx = s_icy.meta_got + (int)i;
                if (idx < (int)sizeof(s_icy.meta) - 1) {
                    s_icy.meta[idx] = (char)p[i];
                }
            }
            s_icy.meta_got += (int)take;
            p += take;
            n -= take;
            if (s_icy.meta_got >= s_icy.meta_len) {
                int term = s_icy.meta_len < (int)sizeof(s_icy.meta) - 1 ? s_icy.meta_len : (int)sizeof(s_icy.meta) - 1;
                s_icy.meta[term] = '\0';
                parse_icy_title(s_icy.meta);
                s_icy.state = ICY_AUDIO;
                s_icy.bytes_to_meta = s_icy.metaint;
            }
            break;
        }
        default:
            s_icy.state = ICY_AUDIO;
            s_icy.bytes_to_meta = s_icy.metaint;
            break;
        }
    }
    return true;
}

/* Open the connection, follow redirects manually, require HTTP 200. */
static esp_err_t open_stream(esp_http_client_handle_t client)
{
    for (int hop = 0; hop <= APP_HTTP_MAX_REDIRECTS; hop++) {
        if (abort_requested()) {
            return ESP_ERR_INVALID_STATE;
        }
        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "open failed: %s", esp_err_to_name(err));
            return err;
        }
        int64_t content_length = esp_http_client_fetch_headers(client);
        if (content_length < 0) {
            ESP_LOGW(TAG, "fetch_headers failed");
            esp_http_client_close(client);
            return ESP_FAIL;
        }
        int status = esp_http_client_get_status_code(client);
        if (status == 200) {
            return ESP_OK;
        }
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            err = esp_http_client_set_redirection(client);
            esp_http_client_close(client);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "redirect without usable Location: %s", esp_err_to_name(err));
                return err;
            }
            ESP_LOGI(TAG, "redirect %d/%d (HTTP %d)", hop + 1, APP_HTTP_MAX_REDIRECTS, status);
            /* Header values from the redirect response are irrelevant. */
            memset(&s_icy, 0, sizeof(s_icy));
            continue;
        }
        ESP_LOGW(TAG, "HTTP status %d", status);
        esp_http_client_close(client);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_ERR_HTTP_MAX_REDIRECT;
}

/* Read until abort, error, server close or idle limit. */
static void read_loop(esp_http_client_handle_t client)
{
    int64_t last_data_us = esp_timer_get_time();
    while (!abort_requested()) {
        int n = esp_http_client_read(client, (char *)s_rx, sizeof(s_rx));
        if (n > 0) {
            last_data_us = esp_timer_get_time();
            if (!process_chunk(s_rx, (size_t)n)) {
                return;     /* abort requested while pushing */
            }
            continue;
        }
        if (n == 0) {
            if (esp_http_client_is_complete_data_received(client)) {
                ESP_LOGW(TAG, "server closed the stream");
                return;
            }
            /* No data, no error: avoid a tight loop. */
            vTaskDelay(pdMS_TO_TICKS(10));
        } else if (n == -ESP_ERR_HTTP_EAGAIN) {
            /* socket read timeout, nothing arrived within APP_HTTP_TIMEOUT_MS */
        } else {
            ESP_LOGW(TAG, "read error %d (errno %d)", n, esp_http_client_get_errno(client));
            return;
        }
        if ((esp_timer_get_time() - last_data_us) > (int64_t)APP_HTTP_IDLE_LIMIT_MS * 1000) {
            ESP_LOGW(TAG, "no data for %d ms, reconnecting", APP_HTTP_IDLE_LIMIT_MS);
            return;
        }
    }
}

static void run_session(void)
{
    uint32_t backoff_ms = APP_HTTP_BACKOFF_MIN_MS;
    uint32_t failures = 0;
    bool first_attempt = true;

    esp_http_client_config_t cfg = {
        .url = s_url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = APP_HTTP_CONNECT_TIMEOUT_MS,   /* lowered to the read timeout once connected */
        .buffer_size = APP_HTTP_RX_BUFFER,
        .buffer_size_tx = APP_HTTP_TX_BUFFER,
        .event_handler = http_event_cb,
        .user_agent = APP_HTTP_USER_AGENT,
        .crt_bundle_attach = esp_crt_bundle_attach,   /* TLS verification via certificate bundle */
        .max_redirection_count = APP_HTTP_MAX_REDIRECTS,
        .keep_alive_enable = false,
    };

    while (!abort_requested()) {
        app_status_set_playback(PB_CONNECTING);
        if (!first_attempt) {
            app_status_inc_reconnects();
        }
        first_attempt = false;

        memset(&s_icy, 0, sizeof(s_icy));
        app_status_set_stream_title("");        /* new connection: title unknown until ICY block */
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (client == NULL) {
            app_status_set_error("http client init failed");
            wait_abortable(backoff_ms);
            continue;
        }
        esp_http_client_set_header(client, "Icy-MetaData", "1");
        esp_http_client_set_header(client, "Accept", "audio/mpeg, */*");

        esp_err_t err = open_stream(client);
        if (err == ESP_OK) {
            backoff_ms = APP_HTTP_BACKOFF_MIN_MS;
            failures = 0;
            /* Connected: short read timeout keeps station changes responsive. */
            esp_http_client_set_timeout_ms(client, APP_HTTP_TIMEOUT_MS);
            s_icy.state = ICY_AUDIO;
            s_icy.bytes_to_meta = s_icy.metaint;
            s_connected = true;
            ESP_LOGI(TAG, "connected: name='%s' genre='%s' br=%d metaint=%d",
                     s_icy.icy_name, s_icy.icy_genre, s_icy.icy_br, s_icy.metaint);
            /* The station list name is shown on the dashboard; the ICY name is only logged. */
            app_status_set_playback(PB_BUFFERING);
            read_loop(client);
            s_connected = false;
        } else if (err != ESP_ERR_INVALID_STATE) {
            char msg[64];
            snprintf(msg, sizeof(msg), "connect failed: %s", esp_err_to_name(err));
            app_status_set_error(msg);
        }

        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        if (abort_requested()) {
            break;
        }
        /* A stream that was delivering data and then dropped is retried at
         * once (servers close idle/rotated connections). Connect failures get
         * a few quick retries (a congested link usually recovers within
         * seconds) and only then back off exponentially. */
        uint32_t delay_ms;
        if (err == ESP_OK) {
            delay_ms = APP_HTTP_FIRST_RETRY_MS;
        } else {
            failures++;
            delay_ms = backoff_ms;
            if (failures >= APP_HTTP_BACKOFF_FAST_RETRIES) {
                backoff_ms *= 2;
                if (backoff_ms > APP_HTTP_BACKOFF_MAX_MS) {
                    backoff_ms = APP_HTTP_BACKOFF_MAX_MS;
                }
            }
        }
        ESP_LOGW(TAG, "reconnect in %lu ms (failures %lu)", (unsigned long)delay_ms, (unsigned long)failures);
        wait_abortable(delay_ms);
    }
}

static void http_stream_task(void *arg)
{
    (void)arg;
    for (;;) {
        s_connected = false;
        s_active = false;
        xEventGroupSetBits(s_events, EV_IDLE);
        xEventGroupWaitBits(s_events, EV_START, pdTRUE, pdFALSE, portMAX_DELAY);
        xEventGroupClearBits(s_events, EV_IDLE);
        s_active = true;
        ESP_LOGI(TAG, "start: %s", s_url);
        run_session();
        ESP_LOGI(TAG, "session ended");
    }
}

/* ---- public API --------------------------------------------------------- */
esp_err_t stream_manager_init(void)
{
    s_sb = xStreamBufferCreateStatic(APP_STREAM_BUFFER_SIZE, 1, s_sb_storage, &s_sb_struct);
    if (s_sb == NULL) {
        ESP_LOGE(TAG, "stream buffer create failed");
        return ESP_ERR_NO_MEM;
    }
    s_events = xEventGroupCreateStatic(&s_events_buf);
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    BaseType_t ok = xTaskCreate(http_stream_task, "http_stream", APP_TASK_STACK_STREAM, NULL,
                                APP_TASK_PRIO_STREAM, &s_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ready, ring buffer %d KB (static .bss)", APP_STREAM_BUFFER_SIZE / 1024);
    return ESP_OK;
}

esp_err_t stream_manager_start(const char *url)
{
    if (url == NULL || strlen(url) >= APP_STREAM_URL_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((xEventGroupGetBits(s_events) & EV_IDLE) == 0) {
        return ESP_ERR_INVALID_STATE;   /* stop() must complete first */
    }
    strncpy(s_url, url, sizeof(s_url) - 1);
    s_url[sizeof(s_url) - 1] = '\0';
    xEventGroupClearBits(s_events, EV_ABORT);
    xEventGroupSetBits(s_events, EV_START);
    return ESP_OK;
}

esp_err_t stream_manager_stop(uint32_t timeout_ms)
{
    if (xEventGroupGetBits(s_events) & EV_IDLE) {
        return ESP_OK;
    }
    xEventGroupSetBits(s_events, EV_ABORT);
    EventBits_t bits = xEventGroupWaitBits(s_events, EV_IDLE, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if ((bits & EV_IDLE) == 0) {
        ESP_LOGE(TAG, "stop timeout: task still inside a blocking call");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t stream_manager_reset_buffer(void)
{
    if ((xEventGroupGetBits(s_events) & EV_IDLE) == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    /* xStreamBufferReset fails if any task is blocked on the buffer. */
    return xStreamBufferReset(s_sb) == pdPASS ? ESP_OK : ESP_ERR_INVALID_STATE;
}

size_t stream_manager_read(uint8_t *dst, size_t max_len, uint32_t timeout_ms)
{
    return xStreamBufferReceive(s_sb, dst, max_len, pdMS_TO_TICKS(timeout_ms));
}

esp_err_t stream_manager_set_trigger_level(size_t bytes)
{
    if (bytes == 0 || bytes > APP_STREAM_BUFFER_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    return xStreamBufferSetTriggerLevel(s_sb, bytes) == pdTRUE ? ESP_OK : ESP_FAIL;
}

size_t stream_manager_bytes_available(void)
{
    return xStreamBufferBytesAvailable(s_sb);
}

size_t stream_manager_capacity(void)
{
    return APP_STREAM_BUFFER_SIZE;
}

bool stream_manager_is_connected(void)
{
    return s_connected;
}

bool stream_manager_is_active(void)
{
    return s_active;
}
