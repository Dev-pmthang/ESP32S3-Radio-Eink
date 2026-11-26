/*
 * audio_player.c - I2S standard-mode master TX for PCM5102 + XSMT control.
 *
 * Clocking: PCM5102 runs with SCK tied to GND, so it derives its internal
 * clock from BCK (PLL mode). We therefore leave MCLK unused and output
 * BCK = 32 x fs (16-bit slots) and LRCK = fs. 32fs/48fs/64fs are all valid
 * BCK ratios for the PCM5102 in this mode.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_attr.h"

#include "app_config.h"
#include "app_status.h"
#include "audio_player.h"

static const char *TAG = "audio_player";

static i2s_chan_handle_t  s_tx = NULL;
static bool               s_running = false;
static volatile bool      s_unmuted = false;
static uint32_t           s_rate = APP_AUDIO_DEFAULT_RATE;
static volatile uint32_t  s_dma_underruns = 0;

/* One DMA descriptor worth of stereo silence (240 frames * 2 ch * 16 bit). */
static const int16_t s_silence[APP_I2S_DMA_FRAME_NUM * 2] = { 0 };

static bool IRAM_ATTR on_send_q_ovf(i2s_chan_handle_t handle, i2s_event_data_t *event, void *user_ctx)
{
    (void)handle;
    (void)event;
    (void)user_ctx;
    s_dma_underruns++;
    return false;   /* no context switch requested */
}

static void preload_silence(void)
{
    /* Fill the whole DMA ring with zeros while the channel is disabled.
     * i2s_channel_preload_data() returns fewer bytes once the ring is full. */
    for (int i = 0; i < APP_I2S_DMA_DESC_NUM + 1; i++) {
        size_t loaded = 0;
        esp_err_t err = i2s_channel_preload_data(s_tx, s_silence, sizeof(s_silence), &loaded);
        if (err != ESP_OK || loaded < sizeof(s_silence)) {
            break;
        }
    }
}

esp_err_t audio_player_init(void)
{
    /* XSMT first: hardware mute before anything else can click. */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << APP_DAC_XSMT_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "XSMT gpio_config failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(APP_DAC_XSMT_GPIO, 0), TAG, "XSMT set low failed");
    s_unmuted = false;
    app_status_set_unmuted(false);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = APP_I2S_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = APP_I2S_DMA_FRAME_NUM;
    chan_cfg.auto_clear = true;   /* DMA buffer is zeroed after being sent -> silence on underrun */
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG, "i2s_new_channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(APP_AUDIO_DEFAULT_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,          /* PCM5102 SCK is tied to GND */
            .bclk = APP_I2S_BCK_GPIO,
            .ws   = APP_I2S_WS_GPIO,
            .dout = APP_I2S_DOUT_GPIO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "init_std_mode failed");

    i2s_event_callbacks_t cbs = {
        .on_recv = NULL,
        .on_recv_q_ovf = NULL,
        .on_sent = NULL,
        .on_send_q_ovf = on_send_q_ovf,
    };
    ESP_RETURN_ON_ERROR(i2s_channel_register_event_callback(s_tx, &cbs, NULL), TAG, "register cb failed");

    s_rate = APP_AUDIO_DEFAULT_RATE;
    s_running = false;
    preload_silence();
    ESP_LOGI(TAG, "I2S0 ready: BCK=%d WS=%d DOUT=%d XSMT=%d, %lu Hz, 16-bit stereo",
             APP_I2S_BCK_GPIO, APP_I2S_WS_GPIO, APP_I2S_DOUT_GPIO, APP_DAC_XSMT_GPIO,
             (unsigned long)s_rate);
    return ESP_OK;
}

esp_err_t audio_player_set_sample_rate(uint32_t sample_rate)
{
    if (s_tx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sample_rate < 8000 || sample_rate > 48000) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_running) {
        ESP_RETURN_ON_ERROR(i2s_channel_disable(s_tx), TAG, "disable failed");
        s_running = false;
    }
    if (sample_rate != s_rate) {
        i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
        ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_clock(s_tx, &clk_cfg), TAG, "reconfig clock failed");
        s_rate = sample_rate;
        ESP_LOGI(TAG, "sample rate -> %lu Hz", (unsigned long)sample_rate);
    }
    preload_silence();
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "enable failed");
    s_running = true;
    return ESP_OK;
}

esp_err_t audio_player_start(uint32_t sample_rate)
{
    if (s_running && sample_rate == s_rate) {
        return ESP_OK;
    }
    return audio_player_set_sample_rate(sample_rate);
}

esp_err_t audio_player_write(const int16_t *stereo_pcm, size_t frames, uint32_t timeout_ms)
{
    if (!s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t *p = (const uint8_t *)stereo_pcm;
    size_t remaining = frames * 2 * sizeof(int16_t);
    while (remaining > 0) {
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx, p, remaining, &written, timeout_ms);
        if (err != ESP_OK) {
            return err;
        }
        p += written;
        remaining -= written;
    }
    return ESP_OK;
}

esp_err_t audio_player_write_silence(uint32_t ms)
{
    if (!s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t frames = (size_t)((uint64_t)s_rate * ms / 1000U);
    while (frames > 0) {
        size_t n = frames > APP_I2S_DMA_FRAME_NUM ? APP_I2S_DMA_FRAME_NUM : frames;
        esp_err_t err = audio_player_write(s_silence, n, APP_I2S_WRITE_TIMEOUT_MS);
        if (err != ESP_OK) {
            return err;
        }
        frames -= n;
    }
    return ESP_OK;
}

void audio_player_set_unmute(bool unmute)
{
    gpio_set_level(APP_DAC_XSMT_GPIO, unmute ? 1 : 0);
    s_unmuted = unmute;
    app_status_set_unmuted(unmute);
}

void audio_player_stop(void)
{
    audio_player_set_unmute(false);
    if (s_running) {
        /* Push one DMA ring of zeros so no stale PCM is left in the
         * descriptors, then stop the clocks. No write happens after disable. */
        (void)audio_player_write_silence(APP_AUDIO_SILENCE_PRE_MS);
        esp_err_t err = i2s_channel_disable(s_tx);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "i2s_channel_disable: %s", esp_err_to_name(err));
        }
        s_running = false;
    }
}

bool audio_player_is_unmuted(void)
{
    return s_unmuted;
}

bool audio_player_is_running(void)
{
    return s_running;
}

uint32_t audio_player_get_sample_rate(void)
{
    return s_rate;
}

uint32_t audio_player_get_dma_underruns(void)
{
    return s_dma_underruns;
}
