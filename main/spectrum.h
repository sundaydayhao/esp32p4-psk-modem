#pragma once

#include <stddef.h>
#include <stdint.h>

#define SPECTRUM_BINS 64
#define SPECTRUM_MAX_SAMPLES 256

typedef struct {
    uint8_t bins[SPECTRUM_BINS];
    int peak_bin;
    int peak_hz;
} spectrum_t;

void spectrum_analyze(const int16_t *samples, size_t count, int sample_rate_hz, spectrum_t *spectrum);
