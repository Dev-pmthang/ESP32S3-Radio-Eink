/*
 * audio_decoder.c - audio_decoder_task.
 *
 *   stream buffer --> staging buffer --> libhelix-mp3 (fixed point)
 *                 --> volume/ramp/DC-block --> audio_player (I2S DMA) --> PCM5102
 *
 * Why libhelix: ESP32-C5 is RV32IMAC without an FPU. minimp3 (float based)
 * measured at ~70 % CPU while still decoding slower than real time
 * (44 DMA underruns/s). libhelix is integer only and is the decoder used by
 * ESP32-audioI2S on FPU-less ESP32 variants.
 *
 * The task is the sole owner of the I2S channel after boot. All mute /
 * unmute decisions (XSMT) are taken here so the order "silence -> XSMT high
 * -> gain ramp" and "gain ramp -> XSMT low -> silence -> disable" is always
 * respected.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "mp3dec.h"

#include "app_config.h"
#include "app_status.h"
#include "audio_player.h"
#include "stream_manager.h"
#include "audio_decoder.h"

static const char *TAG = "decoder";

#define EV_START    BIT0
#define EV_STOP     BIT1
#define EV_IDLE     BIT2

#define GAIN_ONE    32767           /* Q15 unity gain (kept below 32768 to avoid overflow) */
#define PCM_SAMPLES (MAX_NCHAN * MAX_NGRAN * MAX_NSAMP)   /* 2304 shorts = one stereo frame */

typedef enum {
    FRAME_OK = 0,       /* s_pcm holds a decoded frame            */
    FRAME_NEED_DATA,    /* not enough bytes staged                 */
    FRAME_SKIPPED,      /* junk / bit reservoir warm-up, try again */
} frame_result_t;

/* ---- static allocations (no heap use in the audio loop) ----------------- */
static uint8_t              s_in[APP_MP3_INPUT_BUF_SIZE];
static short                s_pcm[PCM_SAMPLES];
static int16_t              s_out[PCM_SAMPLES];

static StaticEventGroup_t   s_events_buf;
static EventGroupHandle_t   s_events = NULL;
static TaskHandle_t         s_task = NULL;
static HMP3Decoder          s_dec = NULL;      /* allocated once by MP3InitDecoder() */

static volatile uint8_t     s_volume = APP_VOLUME_DEFAULT;
static volatile bool        s_mono = (APP_AUDIO_MONO_DEFAULT != 0);

/* decoder session state (task private) */
static size_t   s_in_fill;      /* bytes valid in s_in                 */
static size_t   s_rd;           /* read offset inside s_in             */
static uint32_t s_rate;         /* current I2S sample rate (0 = none)  */
static int      s_channels;     /* 1 or 2 from the MP3 stream          */
static int32_t  s_gain_cur;     /* Q15 current gain                    */
static int32_t  s_gain_target;  /* Q15 target gain                     */
static int32_t  s_gain_step;    /* Q15 per-sample ramp step            */
static int32_t  s_dc_x[2];
static int32_t  s_dc_y[2];
static int      s_last_samples; /* samples per channel in s_pcm        */
static int      s_last_channels;
static size_t   s_prebuffer;    /* adaptive pre-buffer threshold        */
static MP3FrameInfo s_info;

/* ---- helpers ------------------------------------------------------------ */
static inline bool stop_requested(void)
{
    return (xEventGroupGetBits(s_events) & EV_STOP) != 0;
}

static inline int32_t gain_from_percent(uint8_t pct)
{
    if (pct > 100) {
        pct = 100;
    }
    /* Square law ~ perceptual: 50 % -> -12 dB, 25 % -> -24 dB. */
    return (int32_t)(((uint32_t)pct * pct * GAIN_ONE) / 10000U);
}

static inline int32_t ramp_step_for(uint32_t rate, uint32_t ramp_ms)
{
    uint32_t samples = rate * ramp_ms / 1000U;
    if (samples == 0) {
        return GAIN_ONE;
    }
    int32_t step = GAIN_ONE / (int32_t)samples;
    return step < 1 ? 1 : step;
}

static inline int16_t saturate16(int32_t v)
{
    if (v > 32767) {
        return 32767;
    }
    if (v < -32768) {
        return -32768;
    }
    return (int16_t)v;
}

#if APP_AUDIO_DC_BLOCK
/* One-pole DC blocker: y[n] = x[n] - x[n-1] + y[n-1] * (1 - 1/256).
 * Cut-off ~ 27 Hz at 44.1 kHz, integer only. */
static inline int32_t dc_block(int ch, int32_t x)
{
    int32_t y = x - s_dc_x[ch] + s_dc_y[ch] - (s_dc_y[ch] >> 8);
    s_dc_x[ch] = x;
    s_dc_y[ch] = y;
    return y;
}
#endif

static void reset_dc_state(void)
{
    s_dc_x[0] = s_dc_x[1] = 0;
    s_dc_y[0] = s_dc_y[1] = 0;
}

/* Apply volume ramp (+ mono->stereo, DC block, saturation) from s_pcm to
 * s_out. Returns the peak level 0..100 of the processed block. */
static uint8_t process_pcm(int samples, int channels)
{
    int32_t peak = 0;
    const bool mono = s_mono;
    for (int i = 0; i < samples; i++) {
        int32_t l, r;
        if (channels == 2) {
            l = s_pcm[2 * i];
            r = s_pcm[2 * i + 1];
            if (mono) {
                /* Single speaker on one amplifier channel: sum both sides at
                 * half level so nothing from L is lost and nothing clips. */
                l = (l + r) >> 1;
                r = l;
            }
        } else {
            l = s_pcm[i];
            r = l;
        }
        if (s_gain_cur != s_gain_target) {
            if (s_gain_cur < s_gain_target) {
                s_gain_cur += s_gain_step;
                if (s_gain_cur > s_gain_target) {
                    s_gain_cur = s_gain_target;
                }
            } else {
                s_gain_cur -= s_gain_step;
                if (s_gain_cur < s_gain_target) {
                    s_gain_cur = s_gain_target;
                }
            }
        }
        l = (l * s_gain_cur) >> 15;
        r = (r * s_gain_cur) >> 15;
#if APP_AUDIO_DC_BLOCK
        l = dc_block(0, l);
        r = dc_block(1, r);
#endif
        int16_t ls = saturate16(l);
        int16_t rs = saturate16(r);
        s_out[2 * i] = ls;
        s_out[2 * i + 1] = rs;
        int32_t a = ls < 0 ? -(int32_t)ls : ls;
        int32_t b = rs < 0 ? -(int32_t)rs : rs;
        if (a > peak) {
            peak = a;
        }
        if (b > peak) {
            peak = b;
        }
    }
    return (uint8_t)((peak * 100) / 32767);
}

/* Pull more compressed data into the staging buffer (compacting first). */
static void refill_input(uint32_t timeout_ms, int64_t *last_data_us)
{
    if (s_rd > 0) {
        memmove(s_in, s_in + s_rd, s_in_fill - s_rd);
        s_in_fill -= s_rd;
        s_rd = 0;
    }
    size_t space = sizeof(s_in) - s_in_fill;
    if (space == 0) {
        return;
    }
    size_t n = stream_manager_read(s_in + s_in_fill, space, timeout_ms);
    if (n > 0) {
        s_in_fill += n;
        *last_data_us = esp_timer_get_time();
    }
}

/* Decode one MP3 frame from the staging buffer into s_pcm.
 * On FRAME_OK, *samples = samples per channel and *channels is set. */
static frame_result_t decode_frame(int *samples, int *channels)
{
    size_t avail = s_in_fill - s_rd;
    if (avail < APP_MP3_DECODE_MIN_BYTES) {
        return FRAME_NEED_DATA;
    }
    int sync = MP3FindSyncWord(s_in + s_rd, (int)avail);
    if (sync < 0) {
        /* No sync word in the whole window: drop it (keep 1 byte in case a
         * header straddles the boundary). */
        s_rd += avail - 1;
        app_status_inc_decoder_errors();
        return FRAME_SKIPPED;
    }
    if (sync > 0) {
        s_rd += (size_t)sync;
        avail -= (size_t)sync;
        if (avail < APP_MP3_DECODE_MIN_BYTES) {
            return FRAME_NEED_DATA;
        }
    }
    unsigned char *inp = s_in + s_rd;
    int left = (int)avail;
    int err = MP3Decode(s_dec, &inp, &left, s_pcm, 0);
    size_t consumed = avail - (size_t)left;

    switch (err) {
    case ERR_MP3_NONE:
        s_rd += consumed;
        MP3GetLastFrameInfo(s_dec, &s_info);
        if (s_info.nChans < 1 || s_info.nChans > 2 || s_info.outputSamps <= 0) {
            app_status_inc_decoder_errors();
            return FRAME_SKIPPED;
        }
        *channels = s_info.nChans;
        *samples = s_info.outputSamps / s_info.nChans;
        return FRAME_OK;
    case ERR_MP3_INDATA_UNDERFLOW:
        /* Frame longer than what is staged: wait for more bytes. If the
         * window is already large this is a bogus header -> skip it. */
        if (avail >= sizeof(s_in) / 2) {
            s_rd += 1;
            app_status_inc_decoder_errors();
            return FRAME_SKIPPED;
        }
        return FRAME_NEED_DATA;
    case ERR_MP3_MAINDATA_UNDERFLOW:
        /* Bit reservoir not primed yet (first frames after a seek/start). */
        s_rd += consumed > 0 ? consumed : 1;
        return FRAME_SKIPPED;
    default:
        /* Corrupt frame: step past it and resync. */
        s_rd += consumed > 0 ? consumed : 1;
        app_status_inc_decoder_errors();
        return FRAME_SKIPPED;
    }
}

/* Wait until s_prebuffer bytes are buffered (or timeout with enough for a
 * couple of frames). Returns false when a stop was requested. */
static bool wait_prebuffer(void)
{
    int64_t t0 = esp_timer_get_time();
    bool logged = false;
    while (!stop_requested()) {
        size_t avail = stream_manager_bytes_available() + (s_in_fill - s_rd);
        if (avail >= s_prebuffer) {
            return true;
        }
        int64_t waited_ms = (esp_timer_get_time() - t0) / 1000;
        if (waited_ms > APP_PREBUFFER_TIMEOUT_MS) {
            /* Start early only if the stream is actually connected and still
             * trickling; starting on a dead link just underruns immediately. */
            if (avail >= 2 * APP_MP3_MAX_FRAME_BYTES && stream_manager_is_connected()) {
                ESP_LOGW(TAG, "prebuffer timeout, starting with %u bytes", (unsigned)avail);
                return true;
            }
            if (!stream_manager_is_connected()) {
                t0 = esp_timer_get_time();      /* restart the clock once reconnected */
            }
            if (!logged) {
                ESP_LOGW(TAG, "still waiting for stream data (%u bytes)", (unsigned)avail);
                logged = true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return false;
}

/* Switch I2S clock when the MP3 format changes. Caller guarantees mute. */
static esp_err_t apply_format(uint32_t hz, int channels)
{
    if (audio_player_is_unmuted()) {
        audio_player_set_unmute(false);
    }
    esp_err_t err = audio_player_set_sample_rate(hz);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S reconfig to %lu Hz failed: %s", (unsigned long)hz, esp_err_to_name(err));
        return err;
    }
    s_rate = hz;
    s_channels = channels;
    s_gain_cur = 0;
    s_gain_step = ramp_step_for(s_rate, APP_AUDIO_VOLUME_RAMP_MS);
    reset_dc_state();
    app_status_set_audio_format(s_rate, (uint8_t)channels, (uint16_t)(s_info.bitrate / 1000));
    ESP_LOGI(TAG, "format: %lu Hz, %d ch, %d kbps, MPEG layer %d", (unsigned long)hz, channels,
             s_info.bitrate / 1000, s_info.layer);
    return ESP_OK;
}

/* Fade to zero, hardware mute, flush zeros, disable I2S. */
static void fade_out_and_stop(void)
{
    if (audio_player_is_unmuted() && audio_player_is_running()) {
        s_gain_target = 0;
        s_gain_step = ramp_step_for(s_rate, APP_AUDIO_FADE_OUT_MS);
        /* Prefer decoding a few more real frames so the fade is seamless. */
        for (int k = 0; k < 4 && s_gain_cur > 0; k++) {
            int samples = 0;
            int channels = 0;
            frame_result_t r = decode_frame(&samples, &channels);
            if (r == FRAME_NEED_DATA) {
                break;
            }
            if (r != FRAME_OK || (uint32_t)s_info.samprate != s_rate) {
                continue;
            }
            s_last_samples = samples;
            s_last_channels = channels;
            process_pcm(samples, channels);
            (void)audio_player_write(s_out, (size_t)samples, APP_I2S_WRITE_TIMEOUT_MS);
        }
        /* No fresh data left: decay the last decoded block instead. */
        if (s_gain_cur > 0 && s_last_samples > 0) {
            process_pcm(s_last_samples, s_last_channels);
            (void)audio_player_write(s_out, (size_t)s_last_samples, APP_I2S_WRITE_TIMEOUT_MS);
        }
        (void)audio_player_write_silence(10);
    }
    audio_player_stop();        /* XSMT LOW, zero flush, channel disable */
    s_gain_cur = 0;
    app_status_set_level(0);
}

static void handle_underrun(size_t avail)
{
    /* Hysteresis: every underrun in this session raises the pre-buffer so a
     * slow or bursty server gets more headroom instead of stuttering. */
    if (s_prebuffer + APP_PREBUFFER_STEP_BYTES <= APP_PREBUFFER_MAX_BYTES) {
        s_prebuffer += APP_PREBUFFER_STEP_BYTES;
    } else {
        s_prebuffer = APP_PREBUFFER_MAX_BYTES;
    }
    ESP_LOGW(TAG, "underrun (%u bytes staged, %u in ring) -> pre-buffer now %u B", (unsigned)avail,
             (unsigned)stream_manager_bytes_available(), (unsigned)s_prebuffer);
    app_status_inc_underruns();
    if (audio_player_is_unmuted()) {
        audio_player_set_unmute(false);     /* PCM5102 soft-mutes internally on XSMT */
    }
    if (audio_player_is_running()) {
        (void)audio_player_write_silence(APP_AUDIO_SILENCE_PRE_MS);
    }
    s_gain_cur = 0;
    app_status_set_level(0);
}

/* One playback session: pre-buffer -> play -> (underrun -> pre-buffer) ... */
static void decoder_session(void)
{
    s_in_fill = 0;
    s_rd = 0;
    s_rate = 0;
    s_channels = 0;
    s_gain_cur = 0;
    s_gain_target = gain_from_percent(s_volume);
    s_gain_step = ramp_step_for(APP_AUDIO_DEFAULT_RATE, APP_AUDIO_VOLUME_RAMP_MS);
    s_last_samples = 0;
    s_last_channels = 2;
    s_prebuffer = APP_PREBUFFER_BYTES;      /* fast start; grows on underruns */
    memset(&s_info, 0, sizeof(s_info));
    reset_dc_state();

    while (!stop_requested()) {
        app_status_set_playback(PB_BUFFERING);
        if (audio_player_is_unmuted()) {
            audio_player_set_unmute(false);
        }
        if (!wait_prebuffer()) {
            break;
        }

        int64_t last_data_us = esp_timer_get_time();
        bool announced = false;
        uint8_t level_max = 0;
        int frames = 0;

        while (!stop_requested()) {
            if (s_in_fill - s_rd < APP_MP3_INPUT_LOW_WATER) {
                refill_input(20, &last_data_us);    /* blocks up to 20 ms when the ring is empty */
            }

            int samples = 0;
            int channels = 0;
            frame_result_t r = decode_frame(&samples, &channels);
            if (r == FRAME_NEED_DATA) {
                if ((esp_timer_get_time() - last_data_us) > (int64_t)APP_UNDERRUN_TIMEOUT_MS * 1000) {
                    handle_underrun(s_in_fill - s_rd);
                    break;      /* back to pre-buffering */
                }
                continue;       /* refill_input above blocks: not a busy loop */
            }
            if (r == FRAME_SKIPPED) {
                continue;
            }
            s_last_samples = samples;
            s_last_channels = channels;

            uint32_t hz = (uint32_t)s_info.samprate;
            if (hz != s_rate || channels != s_channels || !audio_player_is_running()) {
                if (hz != 44100 && hz != 48000 && hz != 32000 && hz != 22050 && hz != 24000 && hz != 16000) {
                    ESP_LOGW(TAG, "unsupported sample rate %lu", (unsigned long)hz);
                    app_status_inc_decoder_errors();
                    continue;
                }
                if (apply_format(hz, channels) != ESP_OK) {
                    app_status_set_error("I2S reconfig failed");
                    audio_player_stop();
                    return;
                }
            }

            s_gain_target = gain_from_percent(s_volume);
            uint8_t level = process_pcm(samples, channels);

            if (!audio_player_is_unmuted() && s_gain_target > 0) {
                /* Zero-pop unmute: digital silence on the bus, then XSMT HIGH,
                 * then the gain ramp (s_gain_cur starts at 0) fades in. */
                (void)audio_player_write_silence(APP_AUDIO_SILENCE_PRE_MS);
                audio_player_set_unmute(true);
            }

            esp_err_t werr = audio_player_write(s_out, (size_t)samples, APP_I2S_WRITE_TIMEOUT_MS);
            if (werr != ESP_OK) {
                ESP_LOGW(TAG, "i2s write: %s", esp_err_to_name(werr));
                app_status_inc_decoder_errors();
            }

            if (audio_player_is_unmuted() && s_gain_target == 0 && s_gain_cur == 0) {
                audio_player_set_unmute(false);     /* volume 0 => hardware mute */
            }

            if (!announced) {
                app_status_set_playback(PB_PLAYING);
                announced = true;
            }
            if (level > level_max) {
                level_max = level;
            }
            if (++frames >= 8) {        /* ~200 ms at 44.1 kHz */
                app_status_set_level(audio_player_is_unmuted() ? level_max : 0);
                level_max = 0;
                frames = 0;
            }
        }
    }
    fade_out_and_stop();
}

static void audio_decoder_task(void *arg)
{
    (void)arg;
    for (;;) {
        xEventGroupSetBits(s_events, EV_IDLE);
        xEventGroupWaitBits(s_events, EV_START, pdTRUE, pdFALSE, portMAX_DELAY);
        xEventGroupClearBits(s_events, EV_IDLE);
        ESP_LOGI(TAG, "session start (volume %u%%)", (unsigned)s_volume);
        decoder_session();
        ESP_LOGI(TAG, "session end");
    }
}

/* ---- public API --------------------------------------------------------- */
esp_err_t audio_decoder_init(void)
{
    s_dec = MP3InitDecoder();       /* one-time allocation (~24 KB) */
    if (s_dec == NULL) {
        ESP_LOGE(TAG, "MP3InitDecoder failed (no memory)");
        return ESP_ERR_NO_MEM;
    }
    s_events = xEventGroupCreateStatic(&s_events_buf);
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    BaseType_t ok = xTaskCreate(audio_decoder_task, "audio_decoder", APP_TASK_STACK_DECODER, NULL,
                                APP_TASK_PRIO_DECODER, &s_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return ESP_ERR_NO_MEM;
    }
    app_status_set_volume(s_volume);
    ESP_LOGI(TAG, "ready: libhelix-mp3 (fixed point), staging %u B, pcm %u B",
             (unsigned)sizeof(s_in), (unsigned)sizeof(s_pcm));
    return ESP_OK;
}

esp_err_t audio_decoder_start(void)
{
    if ((xEventGroupGetBits(s_events) & EV_IDLE) == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    xEventGroupClearBits(s_events, EV_STOP);
    xEventGroupSetBits(s_events, EV_START);
    return ESP_OK;
}

esp_err_t audio_decoder_stop(uint32_t timeout_ms)
{
    if (xEventGroupGetBits(s_events) & EV_IDLE) {
        return ESP_OK;
    }
    xEventGroupSetBits(s_events, EV_STOP);
    EventBits_t bits = xEventGroupWaitBits(s_events, EV_IDLE, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if ((bits & EV_IDLE) == 0) {
        ESP_LOGE(TAG, "stop timeout");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void audio_decoder_set_volume(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    s_volume = percent;
    app_status_set_volume(percent);
}

uint8_t audio_decoder_get_volume(void)
{
    return s_volume;
}

void audio_decoder_set_mono(bool mono)
{
    s_mono = mono;
    app_status_set_mono(mono);
}

bool audio_decoder_get_mono(void)
{
    return s_mono;
}
