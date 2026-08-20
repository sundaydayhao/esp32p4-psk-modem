#pragma once

#include <stddef.h>
#include <stdint.h>

#include "modem_types.h"

typedef enum {
    DEMOD_FAMILY_PSK,
    DEMOD_FAMILY_FSK,
    DEMOD_FAMILY_MFSK,
    DEMOD_FAMILY_CW,
    DEMOD_FAMILY_FT
} demod_family_t;

typedef struct {
    modem_mode_t mode;
    demod_family_t family;
    float symbol_rate;
    int tone_count;
    int tone_spacing_hz;
    int bandwidth_hz;
} demod_profile_t;

typedef struct {
    float power;
    float i;
    float q;
} tone_power_t;

const demod_profile_t *demod_profile_for_mode(modem_mode_t mode);
float demod_goertzel_power(const int16_t *samples, size_t count, int sample_rate_hz, int freq_hz);
tone_power_t demod_tone_power(const int16_t *samples, size_t count, int sample_rate_hz, int freq_hz);
int demod_clamp_tone(int tone_hz);
