#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t audio_input_init(void);
size_t audio_input_read(int16_t *samples, size_t max_samples);
