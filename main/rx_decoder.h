#pragma once

#include <stddef.h>
#include <stdint.h>

#include "demod_primitives.h"
#include "psk_modes.h"
#include "psk_rx.h"
#include "psk_rtty.h"

typedef struct decoder_layer decoder_layer_t;

typedef void (*decoder_layer_emit_cb_t)(const uint8_t *data, size_t len, void *userdata);

typedef struct {
    modem_mode_t mode;
    psk_mode_t psk_mode;
    int sample_rate_hz;
    int rx_tone_hz;
    int squelch;
    decoder_layer_emit_cb_t emit_cb;
    void *userdata;
} decoder_layer_config_t;

void decoder_layer_init(decoder_layer_t *d, const decoder_layer_config_t *config);
void decoder_layer_set_mode(decoder_layer_t *d, modem_mode_t mode);
void decoder_layer_set_psk_mode(decoder_layer_t *d, psk_mode_t mode);
void decoder_layer_set_tone(decoder_layer_t *d, int tone_hz);
void decoder_layer_feed(decoder_layer_t *d, const int16_t *samples, size_t count);

struct decoder_layer {
    decoder_layer_config_t config;
    const demod_profile_t *profile;
    psk_rx_t psk_rx;
    psk_rtty_rx_t psk_rtty_rx;
    uint32_t symbol_samples;
    int16_t symbol_buf[640];
    void *ft_monitor;
    size_t ft_frame_len;
    float ft_frame[1280];
    int cw_mark;
    int cw_units;
    uint32_t cw_unit_samples;
    uint16_t cw_code;
    uint8_t cw_len;
    uint8_t mfsk_interleave[4 * 4 * 10];
    uint8_t mfsk_symbol_pair[2];
    uint8_t mfsk_symbol_phase;
    uint32_t mfsk_data_shift;
    uint8_t mfsk_viterbi_ptr;
    int16_t mfsk_viterbi_metric[256][64];
    uint8_t mfsk_viterbi_history[256][64];
    uint8_t mfsk_viterbi_sequence[256];
};
