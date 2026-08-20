#include "audio_decim.h"

static int16_t clamp_i16(int32_t v)
{
    if (v > 32767) {
        return 32767;
    }
    if (v < -32768) {
        return -32768;
    }
    return (int16_t)v;
}

void audio_decim_init(audio_decim_state_t *state)
{
    if (state) {
        for (size_t i = 0; i < sizeof(state->fir_hist) / sizeof(state->fir_hist[0]); ++i) {
            state->fir_hist[i] = 0;
        }
        state->dc_x1 = 0;
        state->dc_y1 = 0;
        state->phase = 0;
        state->fir_pos = 0;
    }
}

size_t audio_decim_boxcar(const int16_t *in, size_t in_count, int16_t *out, uint32_t decimation)
{
    size_t out_count = 0;

    for (size_t base = 0; base + decimation <= in_count; base += decimation) {
        int32_t acc = 0;
        for (uint32_t j = 0; j < decimation; ++j) {
            acc += in[base + j];
        }
        out[out_count++] = (int16_t)(acc / (int32_t)decimation);
    }

    return out_count;
}

size_t audio_decim_process(audio_decim_state_t *state, const int16_t *in, size_t in_count,
                           int16_t *out, uint32_t decimation)
{
    if (!state || !in || !out || decimation == 0U) {
        return 0U;
    }

    size_t out_count = 0U;
    for (size_t i = 0; i < in_count; ++i) {
        static const int16_t coef[7] = {1, 2, 3, 4, 3, 2, 1};
        state->fir_hist[state->fir_pos] = in[i];
        if (++state->fir_pos >= 7U) {
            state->fir_pos = 0U;
        }

        if (++state->phase < decimation) {
            continue;
        }
        state->phase = 0U;

        int32_t acc = 0;
        uint32_t pos = state->fir_pos;
        for (uint32_t tap = 0; tap < 7U; ++tap) {
            if (pos == 0U) {
                pos = 7U;
            }
            --pos;
            acc += (int32_t)state->fir_hist[pos] * coef[tap];
        }

        const int32_t y = acc >> 4;
        int32_t dc = y - state->dc_x1 + state->dc_y1 - (state->dc_y1 >> 8);
        state->dc_x1 = y;
        state->dc_y1 = dc;
        out[out_count++] = clamp_i16(dc);
    }

    return out_count;
}
