#include "demod_primitives.h"

#include <math.h>

#define TWO_PI_F 6.28318530717958647692f

static const demod_profile_t s_profiles[MODEM_MODE_COUNT] = {
    [MODEM_MODE_BPSK31] = {MODEM_MODE_BPSK31, DEMOD_FAMILY_PSK, 31.25f, 1, 0, 2200},
    [MODEM_MODE_PSK63] = {MODEM_MODE_PSK63, DEMOD_FAMILY_PSK, 62.5f, 1, 0, 2200},
    [MODEM_MODE_FT8] = {MODEM_MODE_FT8, DEMOD_FAMILY_FT, 6.25f, 8, 6, 2800},
    [MODEM_MODE_FT4] = {MODEM_MODE_FT4, DEMOD_FAMILY_FT, 20.833333f, 4, 20, 2800},
    [MODEM_MODE_RTTY] = {MODEM_MODE_RTTY, DEMOD_FAMILY_FSK, 45.45f, 2, 170, 2200},
    [MODEM_MODE_CW] = {MODEM_MODE_CW, DEMOD_FAMILY_CW, 12.0f, 1, 0, 2200},
    [MODEM_MODE_MFSK16] = {MODEM_MODE_MFSK16, DEMOD_FAMILY_MFSK, 15.625f, 16, 15, 2200},
};

const demod_profile_t *demod_profile_for_mode(modem_mode_t mode)
{
    if (mode >= MODEM_MODE_COUNT) {
        mode = MODEM_MODE_BPSK31;
    }
    return &s_profiles[mode];
}

int demod_clamp_tone(int tone_hz)
{
    if (tone_hz < 100) {
        return 100;
    }
    if (tone_hz > 3500) {
        return 3500;
    }
    return tone_hz;
}

tone_power_t demod_tone_power(const int16_t *samples, size_t count, int sample_rate_hz, int freq_hz)
{
    tone_power_t out = {0};
    if (!samples || count == 0 || sample_rate_hz <= 0) {
        return out;
    }

    const float w = TWO_PI_F * (float)freq_hz / (float)sample_rate_hz;
    const float rot_c = cosf(w);
    const float rot_s = sinf(w);
    float c = 1.0f;
    float s = 0.0f;

    for (size_t i = 0; i < count; ++i) {
        const float window = count > 1 ? 0.5f - 0.5f * cosf(TWO_PI_F * (float)i / (float)(count - 1)) : 1.0f;
        const float x = ((float)samples[i] / 32768.0f) * window;
        out.i += x * c;
        out.q += x * s;

        const float next_c = c * rot_c - s * rot_s;
        s = s * rot_c + c * rot_s;
        c = next_c;
    }
    out.power = out.i * out.i + out.q * out.q;
    return out;
}

float demod_goertzel_power(const int16_t *samples, size_t count, int sample_rate_hz, int freq_hz)
{
    return demod_tone_power(samples, count, sample_rate_hz, freq_hz).power;
}
