#include "spectrum.h"

#include <math.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define SPECTRUM_BSS EXT_RAM_BSS_ATTR
#else
#define SPECTRUM_BSS
#endif

#define TWO_PI 6.28318530717958647692f
#define SPECTRUM_DISPLAY_SPAN_DB 60.0f
#define BLACKMAN_COHERENT_GAIN 0.42f
#define SPECTRUM_POWER_EPSILON 1.0e-10f

typedef struct {
    int sample_rate_hz;
    float window[SPECTRUM_FFT_SIZE];
    float twiddle_c[SPECTRUM_FFT_SIZE / 2U];
    float twiddle_s[SPECTRUM_FFT_SIZE / 2U];
    uint8_t ready;
} spectrum_tables_t;

static spectrum_tables_t s_tables SPECTRUM_BSS;
static float s_fft_real[SPECTRUM_FFT_SIZE] SPECTRUM_BSS;
static float s_fft_imag[SPECTRUM_FFT_SIZE] SPECTRUM_BSS;
static int16_t s_history[SPECTRUM_FFT_SIZE] SPECTRUM_BSS;
static size_t s_history_count;

static void prepare_tables(int sample_rate_hz)
{
    if (s_tables.ready && s_tables.sample_rate_hz == sample_rate_hz) {
        return;
    }

    s_tables.sample_rate_hz = sample_rate_hz;
    for (size_t n = 0; n < SPECTRUM_FFT_SIZE; ++n) {
        const float phase = TWO_PI * (float)n / (float)(SPECTRUM_FFT_SIZE - 1U);
        s_tables.window[n] = 0.42f - 0.5f * cosf(phase) + 0.08f * cosf(2.0f * phase);
    }
    for (size_t i = 0; i < SPECTRUM_FFT_SIZE / 2U; ++i) {
        const float w = -TWO_PI * (float)i / (float)SPECTRUM_FFT_SIZE;
        s_tables.twiddle_c[i] = cosf(w);
        s_tables.twiddle_s[i] = sinf(w);
    }
    s_tables.ready = 1U;
}

static void fft_inplace(float *real, float *imag)
{
    for (size_t i = 1U, j = 0U; i < SPECTRUM_FFT_SIZE; ++i) {
        size_t bit = SPECTRUM_FFT_SIZE >> 1U;
        while ((j & bit) != 0U) {
            j ^= bit;
            bit >>= 1U;
        }
        j ^= bit;
        if (i < j) {
            const float tmp_real = real[i];
            const float tmp_imag = imag[i];
            real[i] = real[j];
            imag[i] = imag[j];
            real[j] = tmp_real;
            imag[j] = tmp_imag;
        }
    }

    for (size_t len = 2U; len <= SPECTRUM_FFT_SIZE; len <<= 1U) {
        const size_t half = len >> 1U;
        const size_t twiddle_step = SPECTRUM_FFT_SIZE / len;
        for (size_t base = 0U; base < SPECTRUM_FFT_SIZE; base += len) {
            for (size_t i = 0U; i < half; ++i) {
                const size_t twiddle = i * twiddle_step;
                const float real2 = real[base + i + half];
                const float imag2 = imag[base + i + half];
                const float temp_real = real2 * s_tables.twiddle_c[twiddle] -
                                        imag2 * s_tables.twiddle_s[twiddle];
                const float temp_imag = real2 * s_tables.twiddle_s[twiddle] +
                                        imag2 * s_tables.twiddle_c[twiddle];
                const float real1 = real[base + i];
                const float imag1 = imag[base + i];
                real[base + i] = real1 + temp_real;
                imag[base + i] = imag1 + temp_imag;
                real[base + i + half] = real1 - temp_real;
                imag[base + i + half] = imag1 - temp_imag;
            }
        }
    }
}

void spectrum_reset(void)
{
    memset(s_history, 0, sizeof(s_history));
    s_history_count = 0U;
    s_tables.sample_rate_hz = 0;
}

void spectrum_analyze(const int16_t *samples, size_t count, int sample_rate_hz, spectrum_t *spectrum)
{
    if (!spectrum) {
        return;
    }
    memset(spectrum, 0, sizeof(*spectrum));
    spectrum->peak_dbfs = -120.0f;
    if (!samples || count == 0U || sample_rate_hz <= 0) {
        return;
    }

    if (count > SPECTRUM_BLOCK_SAMPLES) {
        count = SPECTRUM_BLOCK_SAMPLES;
    }
    if (s_tables.sample_rate_hz != sample_rate_hz) {
        spectrum_reset();
    }
    prepare_tables(sample_rate_hz);

    if (count >= SPECTRUM_FFT_SIZE) {
        memcpy(s_history, &samples[count - SPECTRUM_FFT_SIZE], sizeof(s_history));
        s_history_count = SPECTRUM_FFT_SIZE;
    } else {
        memmove(s_history, &s_history[count],
                (SPECTRUM_FFT_SIZE - count) * sizeof(s_history[0]));
        memcpy(&s_history[SPECTRUM_FFT_SIZE - count], samples,
               count * sizeof(s_history[0]));
        if (s_history_count + count >= SPECTRUM_FFT_SIZE) {
            s_history_count = SPECTRUM_FFT_SIZE;
        } else {
            s_history_count += count;
        }
    }

    const float vscale = 2.0f / (float)SPECTRUM_FFT_SIZE;
    for (size_t n = 0; n < SPECTRUM_FFT_SIZE; ++n) {
        s_fft_real[n] = ((float)s_history[n] / 32768.0f) * s_tables.window[n] * vscale;
        s_fft_imag[n] = 0.0f;
    }
    fft_inplace(s_fft_real, s_fft_imag);

    float max_power = 0.0f;
    const uint32_t nyquist_hz = (uint32_t)sample_rate_hz / 2U;
    const size_t max_hz = SPECTRUM_BINS < nyquist_hz ? SPECTRUM_BINS : nyquist_hz;

    for (size_t hz = 1U; hz < max_hz; ++hz) {
        uint32_t fft_bin = (uint32_t)(((uint64_t)hz * SPECTRUM_FFT_SIZE +
                                      (uint32_t)sample_rate_hz / 2U) /
                                      (uint32_t)sample_rate_hz);
        if (fft_bin >= SPECTRUM_FFT_SIZE / 2U) {
            fft_bin = SPECTRUM_FFT_SIZE / 2U - 1U;
        }
        const float power = s_fft_real[fft_bin] * s_fft_real[fft_bin] +
                            s_fft_imag[fft_bin] * s_fft_imag[fft_bin];
        if (power > max_power) {
            max_power = power;
            spectrum->peak_bin = (int)hz;
        }

        const float db = 10.0f * log10f(power + SPECTRUM_POWER_EPSILON);
        const float norm = fminf(1.0f, fmaxf(0.0f,
                                  (db + SPECTRUM_DISPLAY_SPAN_DB) /
                                  SPECTRUM_DISPLAY_SPAN_DB));
        spectrum->bins[hz] = (uint8_t)lrintf(norm * 255.0f);
    }

    if (max_power > SPECTRUM_POWER_EPSILON) {
        const float full_scale_power = BLACKMAN_COHERENT_GAIN * BLACKMAN_COHERENT_GAIN;
        spectrum->peak_hz = spectrum->peak_bin;
        spectrum->peak_dbfs = 10.0f * log10f(max_power / full_scale_power);
    }
}
