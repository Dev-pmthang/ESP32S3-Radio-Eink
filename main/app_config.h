/*
 * app_config.h - compile-time configuration for ESP32-S3 CYBER WEB RADIO (E-Ink).
 *
 * All pin numbers, buffer sizes, task priorities and timing constants live here
 * so every module shares one source of truth.
 */
#pragma once

#include <stdint.h>
#include "driver/gpio.h"

/* ---------------------------------------------------------------------------
 * Hardware pins (ESP32-S3-DevKitM-1 / DevKitC-1, avoiding strapping 0/3/45/46,
 * USB-JTAG 19/20, UART0 43/44, and Flash/Octal-PSRAM 26..37)
 * ------------------------------------------------------------------------- */
#define APP_I2S_BCK_GPIO        GPIO_NUM_4   /* PCM5102 BCK                  */
#define APP_I2S_WS_GPIO         GPIO_NUM_5   /* PCM5102 LRCK                 */
#define APP_I2S_DOUT_GPIO       GPIO_NUM_6   /* PCM5102 DIN                  */
#define APP_DAC_XSMT_GPIO       GPIO_NUM_7   /* PCM5102 XSMT: LOW = mute     */

#define APP_RGB_LED_GPIO        GPIO_NUM_48  /* On-board WS2812 (38 on some DevKitC-1 v1.1) */

/* Waveshare 1.54" e-Paper Module (200x200, SSD1681, 4-wire SPI).
 * Uses native SPI2 (FSPI) pins for CS/MOSI/CLK plus free GPIOs for DC/RST/BUSY. */
#define APP_EPD_ENABLE          1
#define APP_EPD_MOSI_GPIO       GPIO_NUM_11  /* DIN  (SPI2 MOSI / FSPID)     */
#define APP_EPD_CLK_GPIO        GPIO_NUM_12  /* CLK  (SPI2 SCLK / FSPICLK)   */
#define APP_EPD_CS_GPIO         GPIO_NUM_10  /* CS   (SPI2 CS0  / FSPICS0)   */
#define APP_EPD_DC_GPIO         GPIO_NUM_9   /* D/C  (HIGH=data, LOW=cmd)    */
#define APP_EPD_RST_GPIO        GPIO_NUM_8   /* RST  (active LOW)            */
#define APP_EPD_BUSY_GPIO       GPIO_NUM_16  /* BUSY (HIGH = busy)           */
#define APP_EPD_SPI_HZ          (4 * 1000 * 1000)
#define APP_EPD_ROTATION        0            /* 0, 90, 180, 270              */
#define APP_EPD_REFRESH_MS      250          /* state poll period            */
#define APP_EPD_MIN_UPDATE_MS   800          /* debounce rapid knob changes  */
#define APP_EPD_FULL_EVERY      20           /* full refresh every N partial */
#define APP_TASK_PRIO_EPD       3            /* below httpd (4) and stream (5) */
#define APP_TASK_STACK_EPD      4096

#define APP_ENC_CLK_GPIO        GPIO_NUM_1   /* KY-040 CLK (A)               */
#define APP_ENC_DT_GPIO         GPIO_NUM_2   /* KY-040 DT  (B)               */
#define APP_ENC_SW_GPIO         GPIO_NUM_15  /* KY-040 SW  (push)            */

/* ---------------------------------------------------------------------------
 * Audio pipeline
 * ------------------------------------------------------------------------- */
/* Ring buffer between network and decoder. The 64 KB target did not fit:
 * ESP32-C5 exposes 320 KB of HP SRAM to the app and Wi-Fi 6 + TLS + task
 * stacks need ~180 KB of heap, so 32 KB (2 s at 128 kbps) is used. Raise
 * it only after checking "heap free" in the monitor log, or move it to
 * PSRAM on modules that have it. */
#define APP_STREAM_BUFFER_SIZE      (24 * 1024)   /* network -> decoder ring (1.5 s @128k); 32 KB left only 6 KB heap with TFT + HTTPS */
#define APP_PREBUFFER_BYTES         (12 * 1024)   /* first start: 0.75 s @128k   */
#define APP_PREBUFFER_MAX_BYTES     (20 * 1024)   /* after underruns (ring is 24K) */
#define APP_PREBUFFER_STEP_BYTES    (4 * 1024)    /* raise per underrun (hysteresis) */
#define APP_PREBUFFER_TIMEOUT_MS    8000          /* never wait forever         */
#define APP_UNDERRUN_TIMEOUT_MS     400           /* no data -> mute/rebuffer   */
#define APP_MP3_INPUT_BUF_SIZE      8192          /* decoder staging buffer     */
#define APP_MP3_INPUT_LOW_WATER     5120          /* refill below this (> DECODE_MIN) */
#define APP_MP3_MAX_FRAME_BYTES     2304          /* > MAINBUF_SIZE (1940) of libhelix */
/* Never call the decoder with fewer bytes than two maximum frames: it needs
 * the whole frame (plus bit reservoir data) to be present. */
#define APP_MP3_DECODE_MIN_BYTES    (2 * APP_MP3_MAX_FRAME_BYTES + 32)

#define APP_AUDIO_DEFAULT_RATE      44100
#define APP_AUDIO_VOLUME_RAMP_MS    30            /* gain ramp time             */
#define APP_AUDIO_FADE_OUT_MS       20            /* ramp before hardware mute  */
#define APP_AUDIO_SILENCE_PRE_MS    20            /* zeros before XSMT high     */
#define APP_AUDIO_DC_BLOCK          1             /* 1 = enable DC blocker      */

#define APP_I2S_DMA_DESC_NUM        6
#define APP_I2S_DMA_FRAME_NUM       240           /* 240 frames * 4 B = 960 B   */
#define APP_I2S_WRITE_TIMEOUT_MS    300

#define APP_VOLUME_DEFAULT          40            /* percent                    */
#define APP_VOLUME_STEP             5             /* per encoder detent (20 detents = full range) */

/* ---------------------------------------------------------------------------
 * HTTP stream client
 * ------------------------------------------------------------------------- */
#define APP_HTTP_TIMEOUT_MS         2000          /* socket read timeout while streaming */
#define APP_HTTP_CONNECT_TIMEOUT_MS 5000          /* DNS/TCP/TLS per step: measured 2 s fails on a congested link */
#define APP_HTTP_RX_CHUNK           2048          /* bytes per read call        */
#define APP_HTTP_RX_BUFFER          2048          /* esp_http_client buffer     */
#define APP_HTTP_TX_BUFFER          1024
#define APP_HTTP_MAX_REDIRECTS      5
#define APP_HTTP_BACKOFF_MIN_MS     300           /* live radio: retry quickly   */
#define APP_HTTP_BACKOFF_FAST_RETRIES 3           /* this many quick retries before doubling */
#define APP_HTTP_BACKOFF_MAX_MS     30000
#define APP_HTTP_FIRST_RETRY_MS     100           /* server closed a working stream: retry at once */
#define APP_HTTP_IDLE_LIMIT_MS      5000          /* no bytes -> reconnect      */
#define APP_HTTP_USER_AGENT         "ESP32-S3-CyberRadio/1.0"
#define APP_ICY_META_MAX            (16 * 255)    /* ICY metadata block max     */
#define APP_STREAM_URL_MAX          256

/* ---------------------------------------------------------------------------
 * Tasks (single core, FreeRTOS). Wi-Fi task runs at 23, tcpip at 18.
 * ------------------------------------------------------------------------- */
#define APP_TASK_PRIO_DECODER       8
#define APP_TASK_STACK_DECODER      8192          /* libhelix: ~3 KB stack, state on heap */
#define APP_TASK_PRIO_ENCODER       7
#define APP_TASK_STACK_ENCODER      3584          /* measured HWM 772 B free at 2560 with long-press logging */
#define APP_TASK_PRIO_CMD           6
#define APP_TASK_STACK_CMD          3584          /* NVS writes need ~2 KB      */
#define APP_TASK_PRIO_STREAM        5
#define APP_TASK_STACK_STREAM       12288         /* TLS handshake runs here    */
#define APP_HTTPD_PRIO              4
#define APP_HTTPD_STACK             6144          /* cJSON status ~2 KB         */
#define APP_HTTPD_MAX_SOCKETS       2
#define APP_TASK_STACK_MONITOR      3072

#define APP_CMD_QUEUE_LEN           8
#define APP_PIPELINE_STOP_TIMEOUT_MS 4000         /* wait for tasks to idle     */

/* ---------------------------------------------------------------------------
 * Rotary encoder
 * ------------------------------------------------------------------------- */
#define APP_ENC_DEBOUNCE_US         4000          /* 4 ms between detents       */
#define APP_ENC_BTN_DEBOUNCE_US     250000        /* 250 ms between presses     */
#define APP_ENC_RAW_QUEUE_LEN       32
#define APP_ENC_INVERT_DIR          0             /* 1 if clockwise lowers volume */
#define APP_ENC_RESYNC_MS           500           /* re-read volume after idle  */
/* Knob modes: press toggles. Mode 1 = rotate changes volume, mode 2 = rotate
 * changes station. Station detents are coalesced and applied once the knob
 * has been still for APP_ENC_STATION_SETTLE_MS (as in the S3 reference). */
#define APP_ENC_MODE_DEFAULT        0             /* 0 = volume, 1 = station    */
#define APP_ENC_STATION_SETTLE_MS   300

/* ---------------------------------------------------------------------------
 * RGB status LED colours (0..255, scaled by brightness)
 * ------------------------------------------------------------------------- */
#define APP_RGB_LED_BRIGHTNESS      40            /* 0..255, WS2812 is very bright */
#define APP_RGB_VOLUME_R            0             /* knob mode VOLUME: cyan     */
#define APP_RGB_VOLUME_G            200
#define APP_RGB_VOLUME_B            255
#define APP_RGB_STATION_R           255           /* knob mode STATION: orange  */
#define APP_RGB_STATION_G           90
#define APP_RGB_STATION_B           0
#define APP_RGB_AP_R                200           /* local / SoftAP portal: magenta blink */
#define APP_RGB_AP_G                0
#define APP_RGB_AP_B                200
#define APP_RGB_CONNECTING_R        255           /* STA connecting: yellow blink */
#define APP_RGB_CONNECTING_G        180
#define APP_RGB_CONNECTING_B        0
#define APP_RGB_HOLD_R              255           /* knob held >= 1 s: red      */
#define APP_RGB_HOLD_G              0
#define APP_RGB_HOLD_B              0
#define APP_RGB_TICK_MS             250           /* LED state machine period   */
#define APP_RGB_BLINK_SLOW_TICKS    3             /* 750 ms on / 750 ms off     */
#define APP_RGB_BLINK_FAST_TICKS    1             /* 250 ms on / 250 ms off     */

/* Long press on the knob: >= APP_ENC_LONGPRESS_MS clears the Wi-Fi
 * credentials and restarts into the SoftAP portal. Red LED from
 * APP_ENC_LONGPRESS_WARN_MS warns that the hold is being counted. */
#define APP_ENC_LONGPRESS_WARN_MS   1000
#define APP_ENC_LONGPRESS_MS        3000

/* ---------------------------------------------------------------------------
 * Output mix. The user drives ONE speaker from the PAM8610 right channel, so
 * the default is a mono downmix (L+R)/2 on both channels: nothing from the
 * left channel is lost and the level is halved before summing (no clipping).
 * Runtime toggle: POST /api/mono, persisted in NVS.
 * ------------------------------------------------------------------------- */
#define APP_AUDIO_MONO_DEFAULT      1

/* ---------------------------------------------------------------------------
 * Wi-Fi
 * ------------------------------------------------------------------------- */
#define APP_WIFI_NVS_NAMESPACE      "wifi_cfg"
#define APP_WIFI_MAX_RETRY          5
#define APP_WIFI_AP_SSID            "ESP32-S3-Radio"
#define APP_WIFI_AP_PASSWORD        "12345678"  /* >= 8 chars => WPA2, "" => open */
#define APP_WIFI_AP_CHANNEL         1
#define APP_WIFI_AP_MAX_CONN        2
#define APP_WIFI_STA_RETRY_PERIOD_MS 30000        /* background retry in AP mode */
/* Compile-time STA credentials, used only while NVS (wifi_cfg) is empty.
 * Fill in the password below, build and flash; credentials saved later via
 * the web UI (POST /api/wifi) take precedence. Leave SSID "" to disable. */
#define APP_WIFI_STA_DEFAULT_SSID     ""           /* "" = configure through the captive portal */
#define APP_WIFI_STA_DEFAULT_PASSWORD ""
/* Captive portal (WiFiManager style): DNS hijack + redirect while the AP is up. */
#define APP_CAPTIVE_PORTAL_URL      "http://192.168.4.1/"
#define APP_CAPTIVE_DNS_PRIO        3
#define APP_CAPTIVE_DNS_STACK       3072
#define APP_WIFI_SCAN_MAX_RECORDS   20
#define APP_WIFI_SCAN_TIME_MIN_MS   60             /* per channel, active scan */
#define APP_WIFI_SCAN_TIME_MAX_MS   150
#define APP_WIFI_AP_STOP_WHEN_CONNECTED 1         /* leave APSTA once STA has IP */

/* ---------------------------------------------------------------------------
 * Settings persistence
 * ------------------------------------------------------------------------- */
#define APP_SETTINGS_NVS_NAMESPACE  "app_cfg"
#define APP_SETTINGS_SAVE_DELAY_MS  2000
#define APP_AUTOPLAY_ON_CONNECT     1

/* ---------------------------------------------------------------------------
 * Monitoring
 * ------------------------------------------------------------------------- */
#define APP_MONITOR_PERIOD_MS       10000
