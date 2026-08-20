#include "spectrum.h"

#include <math.h>
#include <string.h>

#define TWO_PI 6.28318530717958647692f

void spectrum_analyze(const int16_t *samples, size_t count, int sample_rate_hz, spectrum_t *spectrum)
{
    memset(spectrum, 0, sizeof(*spectrum));
    if (count < 2 || sample_rate_hz <= 0) {
        return;
    }
    if (count > SPECTRUM_MAX_SAMPLES) {
        count = SPECTRUM_MAX_SAMPLES;
    }

    float windowed[SPECTRUM_MAX_SAMPLES];
    for (size_t n = 0; n < count; ++n) {
        const float window = 0.5f - 0.5f * cosf(TWO_PI * (float)n / (float)(count - 1));
        windowed[n] = ((float)samples[n] / 32768.0f) * window;
    }

    float max_mag = 1.0f;
    float mags[SPECTRUM_BINS];

    for (int bin = 0; bin < SPECTRUM_BINS; ++bin) {
        const float freq = ((float)bin + 0.5f) * ((float)sample_rate_hz / 2.0f) / (float)SPECTRUM_BINS;
        const float w = TWO_PI * freq / (float)sample_rate_hz;
        const float rot_c = cosf(w);
        const float rot_s = sinf(w);
        float i_acc = 0.0f;
        float q_acc = 0.0f;
        float c = 1.0f;
        float s = 0.0f;

        for (size_t n = 0; n < count; ++n) {
            const float x = windowed[n];
            i_acc += x * c;
            q_acc += x * s;

            const float next_c = c * rot_c - s * rot_s;
            s = s * rot_c + c * rot_s;
            c = next_c;
        }

        mags[bin] = sqrtf(i_acc * i_acc + q_acc * q_acc);
        if (mags[bin] > max_mag) {
            max_mag = mags[bin];
            spectrum->peak_bin = bin;
        }
    }

    for (int bin = 0; bin < SPECTRUM_BINS; ++bin) {
        const float norm = mags[bin] / max_mag;
        spectrum->bins[bin] = (uint8_t)lrintf(fminf(255.0f, norm * 255.0f));
    }

    spectrum->peak_hz = ((2 * spectrum->peak_bin + 1) * sample_rate_hz) / (4 * SPECTRUM_BINS);
}
