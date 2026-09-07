#pragma once

#include <stddef.h>
#include <stdint.h>

#include "psk_modes.h"

const uint32_t *psk_codec_mfsk_varicode_table(void);
uint32_t psk_codec_varicode_code(uint8_t ch);
void psk_codec_append_bit(uint8_t *bits, size_t *len, uint8_t bit);
void psk_codec_append_varicode_text(uint8_t *bits, size_t *len, const char *text);
void psk_codec_append_mfsk_char(uint8_t *bits, size_t *len, unsigned char ch);
void psk_codec_append_mfsk_text(uint8_t *bits, size_t *len, const char *text);
size_t psk_codec_conv_encode_bits(uint8_t *out, size_t out_cap, const uint8_t *in, size_t in_len,
                                  uint8_t k, uint32_t poly1, uint32_t poly2);
uint8_t psk_codec_conv_encode_bit(uint32_t *state, uint8_t bit, uint8_t k,
                                  uint32_t poly1, uint32_t poly2,
                                  uint8_t out_pair[2]);
void psk_codec_interleave2_reset(uint8_t *table, size_t table_len);
uint8_t psk_codec_interleave2_pair(uint8_t *table, size_t table_len, uint16_t depth,
                                   uint8_t bit0, uint8_t bit1, uint8_t out_pair[2]);

uint8_t psk_codec_phase_index_from_symbol(psk_mode_family_t family, uint8_t fec, uint8_t symbol);
uint8_t psk_codec_symbol_from_phase_index(psk_mode_family_t family, uint8_t fec, uint8_t phase_idx);
float psk_codec_phase_delta(psk_mode_family_t family, uint8_t fec, uint8_t symbol);
