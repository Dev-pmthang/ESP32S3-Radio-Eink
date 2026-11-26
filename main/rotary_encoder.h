/*
 * rotary_encoder.h - KY-040 driver: GPIO ISR -> queue -> decode task.
 *
 * Rotation changes the volume (APP_VOLUME_STEP per detent), the push button
 * toggles stop / resume. Both are forwarded as commands to the command
 * manager; the ISR itself only timestamps the edge and queues it.
 */
#pragma once

#include "esp_err.h"

esp_err_t rotary_encoder_init(void);
