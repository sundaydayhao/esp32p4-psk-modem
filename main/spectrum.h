#pragma once

#include <stddef.h>
#include <stdint.h>

#define SPECTRUM_SAMPLE_RATE_HZ 8000U
#define SPECTRUM_BLOCK_SAMPLES 512U
#define SPECTRUM_FFT_SIZE 8192U
#define SPECTRUM_BINS 4000U
#define SPECTRUM_MAX_SAMPLES SPECTRUM_BLOCK_SAMPLES

typedef struct {
    uint8_t bins[SPECTRUM_BINS];
    int peak_bin;
    int peak_hz;
    float peak_dbfs;
} spectrum_t;

void spectrum_reset(void);
void spectrum_analyze(const int16_t *samples, size_t count, int sample_rate_hz, spectrum_t *spectrum);
