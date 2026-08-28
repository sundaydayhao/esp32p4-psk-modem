#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t audio_input_init(void);
esp_err_t audio_input_set_gain_db(float gain_db);
size_t audio_input_read(int16_t *samples, size_t max_samples, uint32_t timeout_ms);
size_t audio_output_write(const int16_t *samples, size_t count);
size_t audio_output_queued(void);
size_t audio_output_capacity(void);
void audio_output_clear(void);
esp_err_t audio_output_set_codec_gain(uint8_t dac_volume, uint8_t headphone_volume);

/* Capture the post-decimation stream used by the decoder.  This makes the
 * board audio path reproducible in the native WAV regression tool. */
esp_err_t audio_input_capture_start(size_t max_samples);
void audio_input_capture_stop(void);
int audio_input_capture_active(void);
size_t audio_input_capture_count(void);
size_t audio_input_capture_copy(size_t offset, int16_t *samples, size_t max_samples);
