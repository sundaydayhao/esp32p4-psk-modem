#pragma once

#include <stddef.h>
#include <stdint.h>

#include "psk_modes.h"

#define PSK_TX_TEXT_MAX   256U
#define PSK_TX_BIT_MAX    16384U
#define PSK_TX_CARRIER_MAX 12U

typedef struct {
    uint32_t sample_rate_hz;
    int32_t tone_hz;
    uint32_t symbol_samples;
    int16_t amplitude;
    uint16_t preamble_symbols;
    uint16_t postamble_symbols;
    uint16_t preamble_left;
    uint16_t postamble_left;
    char text[PSK_TX_TEXT_MAX];
    size_t text_len;
    uint8_t active;
    uint8_t stage;
    uint8_t family;
    uint8_t carriers;
    uint8_t bits_per_symbol;
    uint8_t fec;
    uint8_t punctured;
    uint8_t mfsk_varicode;
    uint8_t frame_1200f;
    uint8_t dual_fec_branch;
    uint8_t fec_k;
    uint16_t interleave_depth;
    uint32_t fec_poly1;
    uint32_t fec_poly2;
    uint8_t payload_bits[PSK_TX_BIT_MAX];
    size_t payload_len;
    size_t payload_pos;
    uint8_t symbol_bits[PSK_TX_CARRIER_MAX];
    float carrier_phase[PSK_TX_CARRIER_MAX];
    float carrier_step[PSK_TX_CARRIER_MAX];
    float data_phase[PSK_TX_CARRIER_MAX];
    float phase_start[PSK_TX_CARRIER_MAX];
    float phase_target[PSK_TX_CARRIER_MAX];
    uint32_t symbol_sample;
} psk_tx_t;

void psk_tx_init(psk_tx_t *tx, uint32_t sample_rate_hz);
int psk_tx_start_text(psk_tx_t *tx, psk_mode_t mode, int32_t tone_hz,
                      const char *text);
size_t psk_tx_generate(psk_tx_t *tx, int16_t *out, size_t capacity);
int psk_tx_active(const psk_tx_t *tx);
