#include "psk_codec.h"

#include <math.h>
#include <string.h>

#include "PSKTables.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define PSK_CODEC_BIT_MAX 16384U

static const uint32_t s_mfsk_varicode_encode[256] = {
    0x75C, 0x760, 0x768, 0x76C, 0x770, 0x774, 0x778, 0x77C,
    0x0A8, 0x780, 0x7A0, 0x7A8, 0x7AC, 0x0AC, 0x7B0, 0x7B4,
    0x7B8, 0x7BC, 0x7C0, 0x7D0, 0x7D4, 0x7D8, 0x7DC, 0x7E0,
    0x7E8, 0x7EC, 0x7F0, 0x7F4, 0x7F8, 0x7FC, 0x800, 0xA00,
    0x004, 0x1C0, 0x1FC, 0x2D8, 0x2A8, 0x2A0, 0x200, 0x1BC,
    0x1F4, 0x1F0, 0x2B4, 0x1E0, 0x0A0, 0x1D8, 0x1D4, 0x1E8,
    0x0E0, 0x0F0, 0x140, 0x154, 0x174, 0x160, 0x16C, 0x1A0,
    0x180, 0x1AC, 0x1EC, 0x1F8, 0x2C0, 0x1DC, 0x2BC, 0x1D0,
    0x280, 0x0BC, 0x100, 0x0D4, 0x0DC, 0x0B8, 0x0F8, 0x150,
    0x158, 0x0C0, 0x1B4, 0x17C, 0x0F4, 0x0E8, 0x0FC, 0x0D0,
    0x0EC, 0x1B0, 0x0D8, 0x0B4, 0x0B0, 0x15C, 0x1A8, 0x168,
    0x170, 0x178, 0x1B8, 0x2E8, 0x2D0, 0x2EC, 0x2D4, 0x2B0,
    0x2AC, 0x014, 0x060, 0x038, 0x034, 0x008, 0x050, 0x058,
    0x030, 0x018, 0x080, 0x070, 0x02C, 0x040, 0x01C, 0x010,
    0x054, 0x078, 0x020, 0x028, 0x00C, 0x03C, 0x06C, 0x068,
    0x074, 0x05C, 0x07C, 0x2DC, 0x2B8, 0x2E0, 0x2F0, 0xA80,
    0xAA0, 0xAA8, 0xAAC, 0xAB0, 0xAB4, 0xAB8, 0xABC, 0xAC0,
    0xAD0, 0xAD4, 0xAD8, 0xADC, 0xAE0, 0xAE8, 0xAEC, 0xAF0,
    0xAF4, 0xAF8, 0xAFC, 0xB00, 0xB40, 0xB50, 0xB54, 0xB58,
    0xB5C, 0xB60, 0xB68, 0xB6C, 0xB70, 0xB74, 0xB78, 0xB7C,
    0x2F4, 0x2F8, 0x2FC, 0x300, 0x340, 0x350, 0x354, 0x358,
    0x35C, 0x360, 0x368, 0x36C, 0x370, 0x374, 0x378, 0x37C,
    0x380, 0x3A0, 0x3A8, 0x3AC, 0x3B0, 0x3B4, 0x3B8, 0x3BC,
    0x3C0, 0x3D0, 0x3D4, 0x3D8, 0x3DC, 0x3E0, 0x3E8, 0x3EC,
    0x3F0, 0x3F4, 0x3F8, 0x3FC, 0x400, 0x500, 0x540, 0x550,
    0x554, 0x558, 0x55C, 0x560, 0x568, 0x56C, 0x570, 0x574,
    0x578, 0x57C, 0x580, 0x5A0, 0x5A8, 0x5AC, 0x5B0, 0x5B4,
    0x5B8, 0x5BC, 0x5C0, 0x5D0, 0x5D4, 0x5D8, 0x5DC, 0x5E0,
    0x5E8, 0x5EC, 0x5F0, 0x5F4, 0x5F8, 0x5FC, 0x600, 0x680,
    0x6A0, 0x6A8, 0x6AC, 0x6B0, 0x6B4, 0x6B8, 0x6BC, 0x6C0,
    0x6D0, 0x6D4, 0x6D8, 0x6DC, 0x6E0, 0x6E8, 0x6EC, 0x6F0,
    0x6F4, 0x6F8, 0x6FC, 0x700, 0x740, 0x750, 0x754, 0x758,
};

static uint32_t highest_bit_mask(uint32_t value)
{
    uint32_t mask = 1UL << 31;
    while (mask != 0U && (value & mask) == 0U) {
        mask >>= 1U;
    }
    return mask;
}

void psk_codec_append_bit(uint8_t *bits, size_t *len, uint8_t bit)
{
    if (*len < PSK_CODEC_BIT_MAX) {
        bits[*len] = bit ? 1U : 0U;
        ++(*len);
    }
}

const uint32_t *psk_codec_mfsk_varicode_table(void)
{
    return s_mfsk_varicode_encode;
}

uint32_t psk_codec_varicode_code(uint8_t ch)
{
    return (uint32_t)VARICODE_TABLE[ch];
}

static void append_bits_from_code(uint8_t *bits, size_t *len, uint32_t code)
{
    uint32_t mask = highest_bit_mask(code);
    while (mask != 0U) {
        psk_codec_append_bit(bits, len, (code & mask) != 0U);
        mask >>= 1U;
    }
}

void psk_codec_append_varicode_text(uint8_t *bits, size_t *len, const char *text)
{
    for (size_t i = 0U; text[i] != '\0'; ++i) {
        const uint32_t code = VARICODE_TABLE[(unsigned char)text[i]];
        append_bits_from_code(bits, len, code);
        psk_codec_append_bit(bits, len, 0U);
        psk_codec_append_bit(bits, len, 0U);
    }
}

void psk_codec_append_mfsk_char(uint8_t *bits, size_t *len, unsigned char ch)
{
    const uint32_t code = s_mfsk_varicode_encode[ch];
    uint32_t mask = highest_bit_mask(code);
    while (mask != 0U) {
        psk_codec_append_bit(bits, len, (code & mask) != 0U);
        mask >>= 1U;
    }
}

void psk_codec_append_mfsk_text(uint8_t *bits, size_t *len, const char *text)
{
    for (size_t i = 0U; text[i] != '\0'; ++i) {
        psk_codec_append_mfsk_char(bits, len, (unsigned char)text[i]);
    }
}

static uint32_t parity32(uint32_t value)
{
    value ^= value >> 16U;
    value ^= value >> 8U;
    value ^= value >> 4U;
    value &= 0x0fU;
    return (0x6996U >> value) & 1U;
}

size_t psk_codec_conv_encode_bits(uint8_t *out, size_t out_cap, const uint8_t *in, size_t in_len,
                                  uint8_t k, uint32_t poly1, uint32_t poly2)
{
    if (k < 2U || k > 16U) {
        return 0U;
    }

    uint32_t state = 0U;
    size_t pos = 0U;
    for (size_t i = 0U; i < in_len; ++i) {
        if (pos + 2U > out_cap) {
            return 0U;
        }
        uint8_t pair[2];
        (void)psk_codec_conv_encode_bit(&state, in[i], k, poly1, poly2, pair);
        out[pos++] = pair[0];
        out[pos++] = pair[1];
    }
    return pos;
}

uint8_t psk_codec_conv_encode_bit(uint32_t *state, uint8_t bit, uint8_t k,
                                  uint32_t poly1, uint32_t poly2,
                                  uint8_t out_pair[2])
{
    if (!state || !out_pair || k < 2U || k > 16U) {
        return 0U;
    }

    const uint32_t mask = (1UL << k) - 1UL;
    *state = ((*state << 1U) | (uint32_t)(bit & 1U)) & mask;
    out_pair[0] = (uint8_t)parity32(*state & poly1);
    out_pair[1] = (uint8_t)parity32(*state & poly2);
    return 1U;
}

void psk_codec_interleave2_reset(uint8_t *table, size_t table_len)
{
    memset(table, 0, table_len);
}

uint8_t psk_codec_interleave2_pair(uint8_t *table, size_t table_len, uint16_t depth,
                                   uint8_t bit0, uint8_t bit1, uint8_t out_pair[2])
{
    if (!table || !out_pair || depth == 0U || table_len < (size_t)depth * 4U) {
        out_pair[0] = bit0 ? 1U : 0U;
        out_pair[1] = bit1 ? 1U : 0U;
        return 0U;
    }

    uint8_t syms[2] = {bit1 ? 1U : 0U, bit0 ? 1U : 0U};
    for (uint16_t k = 0U; k < depth; ++k) {
        const uint16_t base = (uint16_t)(k * 4U);
        table[base + 0U] = table[base + 1U];
        table[base + 1U] = syms[0];
        table[base + 2U] = table[base + 3U];
        table[base + 3U] = syms[1];
        syms[0] = table[base + 1U];
        syms[1] = table[base + 2U];
    }
    out_pair[0] = syms[1];
    out_pair[1] = syms[0];
    return 1U;
}

uint8_t psk_codec_phase_index_from_symbol(psk_mode_family_t family, uint8_t fec, uint8_t symbol)
{
    if (family == PSK_MODE_FAMILY_BPSK) {
        return (uint8_t)((symbol & 1U) ? 0U : 2U);
    }
    if (family == PSK_MODE_FAMILY_QPSK) {
        /* fldigi USB QPSK: sym=(4-sym)&3, then sym_vec_pos[sym*4].
         * sym_vec_pos[0] is 180 degrees, so encoder symbols map to
         * phase indexes 0/90/180/270 as {2,1,0,3}. */
        static const uint8_t map[4] = {2U, 1U, 0U, 3U};
        return map[symbol & 3U];
    }
    if (fec) {
        /* fldigi graymapped_8psk_pos constellation. */
        static const uint8_t map_fec[8] = {0U, 1U, 3U, 2U, 7U, 6U, 4U, 5U};
        return map_fec[symbol & 7U];
    }
    static const uint8_t map[8] = {4U, 5U, 6U, 7U, 0U, 1U, 2U, 3U};
    return map[symbol & 7U];
}

uint8_t psk_codec_symbol_from_phase_index(psk_mode_family_t family, uint8_t fec, uint8_t phase_idx)
{
    if (family == PSK_MODE_FAMILY_BPSK) {
        return (uint8_t)((phase_idx & 1U) ? 0U : 2U);
    }
    if (family == PSK_MODE_FAMILY_QPSK) {
        /* See psk_codec_phase_index_from_symbol(); the QPSK map is self-inverse. */
        static const uint8_t map[4] = {2U, 1U, 0U, 3U};
        return map[phase_idx & 3U];
    }
    if (fec) {
        /* Inverse of fldigi graymapped_8psk_pos constellation. */
        static const uint8_t map_fec[8] = {0U, 1U, 3U, 2U, 6U, 7U, 5U, 4U};
        return map_fec[phase_idx & 7U];
    }
    static const uint8_t map[8] = {4U, 5U, 6U, 7U, 0U, 1U, 2U, 3U};
    return map[phase_idx & 7U];
}

float psk_codec_phase_delta(psk_mode_family_t family, uint8_t fec, uint8_t symbol)
{
    if (family == PSK_MODE_FAMILY_BPSK) {
        return (symbol & 1U) ? 0.0f : (float)M_PI;
    }
    const float step = (family == PSK_MODE_FAMILY_BPSK) ? (float)M_PI :
                       (family == PSK_MODE_FAMILY_QPSK) ? ((float)M_PI / 2.0f) :
                                                          ((float)M_PI / 4.0f);
    return (float)psk_codec_phase_index_from_symbol(family, fec, symbol) * step;
}
