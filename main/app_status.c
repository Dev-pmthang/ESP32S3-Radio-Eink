#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "app_status.h"

static const char *TAG = "status";

static app_status_t s_status;
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;

static inline void lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static inline void unlock(void) { xSemaphoreGive(s_lock); }

static void copy_str(char *dst, size_t dst_size, const char *src)
{
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

esp_err_t app_status_init(void)
{
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    if (s_lock == NULL) {
        ESP_LOGE(TAG, "mutex create failed");
        return ESP_ERR_NO_MEM;
    }
    memset(&s_status, 0, sizeof(s_status));
    s_status.playback = PB_STOPPED;
    s_status.station_index = -1;
    s_status.display_type = DISPLAY_EPD_154;
    s_status.display_rot = 0;
    s_status.battery_pct = 92;
    return ESP_OK;
}

void app_status_get(app_status_t *out)
{
    lock();
    *out = s_status;
    unlock();
}

void app_status_set_playback(playback_state_t st)
{
    lock();
    s_status.playback = st;
    if (st != PB_ERROR) {
        s_status.error[0] = '\0';
    }
    unlock();
}

playback_state_t app_status_get_playback(void)
{
    lock();
    playback_state_t st = s_status.playback;
    unlock();
    return st;
}

void app_status_set_station(int index, const char *name, const char *genre)
{
    lock();
    s_status.station_index = index;
    copy_str(s_status.station_name, sizeof(s_status.station_name), name);
    copy_str(s_status.genre, sizeof(s_status.genre), genre);
    s_status.stream_title[0] = '\0';
    unlock();
}

void app_status_set_stream_title(const char *title)
{
    lock();
    copy_str(s_status.stream_title, sizeof(s_status.stream_title), title);
    unlock();
}

void app_status_set_error(const char *msg)
{
    lock();
    s_status.playback = PB_ERROR;
    copy_str(s_status.error, sizeof(s_status.error), msg);
    unlock();
}

void app_status_set_audio_format(uint32_t rate, uint8_t channels, uint16_t kbps)
{
    lock();
    s_status.sample_rate = rate;
    s_status.channels = channels;
    s_status.bitrate_kbps = kbps;
    unlock();
}

void app_status_set_volume(uint8_t vol)
{
    lock();
    s_status.volume = vol;
    unlock();
}

void app_status_set_unmuted(bool unmuted)
{
    lock();
    s_status.unmuted = unmuted;
    unlock();
}

void app_status_set_level(uint8_t level)
{
    lock();
    s_status.level = level;
    unlock();
}

void app_status_inc_underruns(void)
{
    lock();
    s_status.underruns++;
    unlock();
}

void app_status_inc_reconnects(void)
{
    lock();
    s_status.reconnects++;
    unlock();
}

void app_status_inc_decoder_errors(void)
{
    lock();
    s_status.decoder_errors++;
    unlock();
}

void app_status_set_encoder_mode(uint8_t mode)
{
    lock();
    s_status.encoder_mode = mode;
    unlock();
}

void app_status_set_mono(bool mono)
{
    lock();
    s_status.mono = mono;
    unlock();
}

void app_status_set_display(display_type_t type, uint16_t rot)
{
    lock();
    s_status.display_type = type;
    s_status.display_rot = rot;
    unlock();
}

void app_status_set_battery(uint8_t pct)
{
    lock();
    s_status.battery_pct = pct > 100 ? 100 : pct;
    unlock();
}
