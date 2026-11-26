/*
 * audio_decoder.h - MP3 decoder / PCM consumer task (audio_decoder_task).
 *
 * Pulls MP3 bytes from the stream manager, decodes with minimp3, applies
 * fixed-point volume with ramping, handles pre-buffering, underrun recovery,
 * sample-rate switching and the zero-pop mute sequence. It is the only task
 * that calls into audio_player after initialisation.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

esp_err_t audio_decoder_init(void);

/* Begin a playback session (pre-buffer, then play). Non-blocking. */
esp_err_t audio_decoder_start(void);

/* Fade out, mute, stop I2S and wait until the task is idle. */
esp_err_t audio_decoder_stop(uint32_t timeout_ms);

/* Volume 0..100 (percent). 0 forces hardware mute after the ramp. */
void    audio_decoder_set_volume(uint8_t percent);
uint8_t audio_decoder_get_volume(void);

/* Mono downmix: both output channels carry (L+R)/2. For a single speaker. */
void    audio_decoder_set_mono(bool mono);
bool    audio_decoder_get_mono(void);
