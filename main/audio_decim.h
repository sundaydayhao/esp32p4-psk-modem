#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int16_t fir_hist[7];
    int32_t dc_x1;
    int32_t dc_y1;
    uint32_t phase;
    uint32_t fir_pos;
} audio_decim_state_t;

void audio_decim_init(audio_decim_state_t *state);

/* Legacy box-car decimator, kept for simple A/B checks. */
size_t audio_decim_boxcar(const int16_t *in, size_t in_count, int16_t *out, uint32_t decimation);

/* Streaming mono audio frontend shared by firmware and host tests.
 * It applies a small low-pass cascade before integer decimation and a
 * single-pole DC blocker after decimation. */
size_t audio_decim_process(audio_decim_state_t *state, const int16_t *in, size_t in_count,
                           int16_t *out, uint32_t decimation);
