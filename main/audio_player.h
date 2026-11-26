/*
 * audio_player.h - exclusive owner of the I2S TX channel and the XSMT pin.
 *
 * Threading contract:
 *   - audio_player_init() is called once from app_main() before any task runs.
 *   - Every other function is called ONLY from the audio decoder task. No
 *     other task may touch the I2S channel. The web server and the command
 *     manager read state through the *_is_* / *_get_* getters, which are
 *     plain volatile reads.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* Create the I2S channel (16-bit stereo, Philips), preload zeros, keep muted. */
esp_err_t audio_player_init(void);

/* Enable the channel at the given sample rate (reconfigures if needed).
 * The DMA ring is preloaded with zeros before enabling so the first samples
 * on the bus are digital silence. */
esp_err_t audio_player_start(uint32_t sample_rate);

/* Change the sample rate. Disables, reconfigures the clock, preloads zeros
 * and re-enables. The caller must already have muted (XSMT LOW). */
esp_err_t audio_player_set_sample_rate(uint32_t sample_rate);

/* Blocking write of interleaved stereo 16-bit frames. */
esp_err_t audio_player_write(const int16_t *stereo_pcm, size_t frames, uint32_t timeout_ms);

/* Write 'ms' milliseconds of digital silence. */
esp_err_t audio_player_write_silence(uint32_t ms);

/* XSMT LOW, flush with zeros, disable the channel. Safe to call when idle. */
void audio_player_stop(void);

/* Drive the PCM5102 XSMT pin. true = unmute (HIGH), false = mute (LOW). */
void audio_player_set_unmute(bool unmute);

bool     audio_player_is_unmuted(void);
bool     audio_player_is_running(void);
uint32_t audio_player_get_sample_rate(void);

/* Number of DMA "send queue overflow" events reported by the I2S driver. */
uint32_t audio_player_get_dma_underruns(void);
