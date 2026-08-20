#include "psk_tx.h"
#include "psk_codec.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum {
    PSK_TX_STAGE_IDLE = 0,
    PSK_TX_STAGE_PREAMBLE,
    PSK_TX_STAGE_TEXT,
    PSK_TX_STAGE_POSTAMBLE,
};

static uint8_t s_tx_raw_bits[PSK_TX_BIT_MAX];
static uint8_t s_tx_punctured_bits[PSK_TX_BIT_MAX];
static uint8_t s_tx_interleave_table[2560];
static char s_tx_frame_text[768];

#define PSK_TX_DEFAULT_AMPLITUDE 9000

static void reset_tx(psk_tx_t *tx)
{
    memset(tx->payload_bits, 0, sizeof(tx->payload_bits));
    memset(tx->symbol_bits, 0, sizeof(tx->symbol_bits));
    memset(tx->carrier_phase, 0, sizeof(tx->carrier_phase));
    memset(tx->carrier_step, 0, sizeof(tx->carrier_step));
    memset(tx->data_phase, 0, sizeof(tx->data_phase));
    memset(tx->phase_start, 0, sizeof(tx->phase_start));
    memset(tx->phase_target, 0, sizeof(tx->phase_target));
    tx->payload_len = 0U;
    tx->payload_pos = 0U;
    tx->symbol_sample = 0U;
    tx->active = 0U;
    tx->stage = PSK_TX_STAGE_IDLE;
}

static void load_mode(const psk_mode_info_t *info, psk_tx_t *tx, int32_t tone_hz)
{
    tx->family = (uint8_t)info->family;
    tx->carriers = info->carriers ? info->carriers : 1U;
    tx->bits_per_symbol = info->bits_per_symbol ? info->bits_per_symbol : 1U;
    tx->fec = info->fec;
    tx->punctured = info->punctured;
    tx->mfsk_varicode = info->mfsk_varicode;
    tx->frame_1200f = info->id == PSK_MODE_8PSK1200F;
    tx->dual_fec_branch = info->dual_fec_branch;
    tx->fec_k = info->fec_k;
    tx->interleave_depth = info->interleave_depth;
    tx->fec_poly1 = info->fec_poly1;
    tx->fec_poly2 = info->fec_poly2;
    tx->tone_hz = tone_hz > 0 ? tone_hz : 1500;
    tx->symbol_samples = (uint32_t)((float)tx->sample_rate_hz / info->baud + 0.5f);
    if (tx->symbol_samples == 0U) {
        tx->symbol_samples = 1U;
    }

    const float spacing = 1.4f * info->baud;
    const float first = (float)tx->tone_hz + (((-1.0f * (float)tx->carriers) + 1.0f) * spacing * 0.5f);
    for (uint8_t car = 0U; car < tx->carriers; ++car) {
        const float freq = first + (float)car * spacing;
        tx->carrier_step[car] = (float)(2.0 * M_PI * (double)freq / (double)tx->sample_rate_hz);
        tx->carrier_phase[car] = 0.0f;
        tx->data_phase[car] = 0.0f;
        tx->phase_start[car] = 0.0f;
        tx->phase_target[car] = 0.0f;
    }
}

static uint32_t crc32_text(const char *text, size_t length)
{
    uint32_t crc = 0xffffffffU;
    for (size_t i = 0U; i < length; ++i) {
        crc ^= (uint8_t)text[i];
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ ((crc & 1U) ? 0xedb88320U : 0U);
        }
    }
    return ~crc;
}

static size_t append_decimal(char *out, size_t pos, size_t cap, uint32_t value)
{
    char tmp[10];
    size_t len = 0U;
    do {
        tmp[len++] = (char)('0' + (value % 10U));
        value /= 10U;
    } while (value != 0U && len < sizeof(tmp));

    while (len != 0U && pos + 1U < cap) {
        out[pos++] = tmp[--len];
    }
    return pos;
}

static size_t append_hex8(char *out, size_t pos, size_t cap, uint32_t value)
{
    static const char hex[] = "0123456789ABCDEF";
    for (int shift = 28; shift >= 0 && pos + 1U < cap; shift -= 4) {
        out[pos++] = hex[(value >> (uint8_t)shift) & 0x0fU];
    }
    return pos;
}

static size_t append_base64(char *out, size_t pos, size_t cap, const char *text, size_t length)
{
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0U; i < length && pos + 5U < cap; i += 3U) {
        const uint32_t b0 = (uint8_t)text[i];
        const uint32_t b1 = (i + 1U < length) ? (uint8_t)text[i + 1U] : 0U;
        const uint32_t b2 = (i + 2U < length) ? (uint8_t)text[i + 2U] : 0U;
        out[pos++] = table[(b0 >> 2U) & 0x3fU];
        out[pos++] = table[((b0 << 4U) | (b1 >> 4U)) & 0x3fU];
        out[pos++] = (i + 1U < length) ? table[((b1 << 2U) | (b2 >> 6U)) & 0x3fU] : '=';
        out[pos++] = (i + 2U < length) ? table[b2 & 0x3fU] : '=';
    }
    return pos;
}

static const char *build_1200f_frame_text(psk_tx_t *tx)
{
    size_t pos = 0U;
    const size_t text_len = tx->text_len;
    const uint32_t crc = crc32_text(tx->text, text_len);
    for (uint8_t repeat = 0U; repeat < 3U; ++repeat) {
        if (pos + 32U >= sizeof(s_tx_frame_text)) {
            return tx->text;
        }
        s_tx_frame_text[pos++] = '~';
        memcpy(&s_tx_frame_text[pos], "H1200,0,", 8U);
        pos += 8U;
        pos = append_decimal(s_tx_frame_text, pos, sizeof(s_tx_frame_text), (uint32_t)text_len);
        if (pos + 2U >= sizeof(s_tx_frame_text)) {
            return tx->text;
        }
        s_tx_frame_text[pos++] = ',';
        pos = append_hex8(s_tx_frame_text, pos, sizeof(s_tx_frame_text), crc);
        if (pos + 2U >= sizeof(s_tx_frame_text)) {
            return tx->text;
        }
        s_tx_frame_text[pos++] = ':';
        pos = append_base64(s_tx_frame_text, pos, sizeof(s_tx_frame_text), tx->text, text_len);
        if (pos + 2U >= sizeof(s_tx_frame_text)) {
            return tx->text;
        }
        s_tx_frame_text[pos++] = '~';
    }
    s_tx_frame_text[pos] = '\0';
    return s_tx_frame_text;
}

static size_t build_payload_stream(psk_tx_t *tx)
{
    size_t raw_len = 0U;
    if (tx->mfsk_varicode) {
        const char *text = tx->frame_1200f ? build_1200f_frame_text(tx) : tx->text;
        if (tx->family == PSK_MODE_FAMILY_8PSK) {
            psk_codec_append_mfsk_char(s_tx_raw_bits, &raw_len, 0U);
        }
        psk_codec_append_mfsk_text(s_tx_raw_bits, &raw_len, text);
        if (tx->family == PSK_MODE_FAMILY_8PSK) {
            psk_codec_append_mfsk_char(s_tx_raw_bits, &raw_len, 0U);
            psk_codec_append_bit(s_tx_raw_bits, &raw_len, 1U);
            for (uint8_t i = 0U; i < 32U; ++i) {
                psk_codec_append_bit(s_tx_raw_bits, &raw_len, 0U);
            }
            psk_codec_append_bit(s_tx_raw_bits, &raw_len, 0U);
        }
    } else {
        psk_codec_append_varicode_text(s_tx_raw_bits, &raw_len, tx->text);
    }

    if (!tx->fec) {
        memcpy(tx->payload_bits, s_tx_raw_bits, raw_len);
        return raw_len;
    }

    size_t encoded_len = psk_codec_conv_encode_bits(tx->payload_bits, sizeof(tx->payload_bits),
                                                    s_tx_raw_bits, raw_len, tx->fec_k,
                                                    tx->fec_poly1, tx->fec_poly2);
    if (encoded_len == 0U) {
        return 0U;
    }

    if (tx->interleave_depth != 0U) {
        size_t interleaved_len = 0U;
        psk_codec_interleave2_reset(s_tx_interleave_table, sizeof(s_tx_interleave_table));
        for (size_t i = 0U; i + 1U < encoded_len; i += 2U) {
            uint8_t out_pair[2];
            (void)psk_codec_interleave2_pair(s_tx_interleave_table, sizeof(s_tx_interleave_table),
                                             tx->interleave_depth,
                                             tx->payload_bits[i], tx->payload_bits[i + 1U],
                                             out_pair);
            if (interleaved_len + 2U > PSK_TX_BIT_MAX) {
                return 0U;
            }
            s_tx_punctured_bits[interleaved_len++] = out_pair[0];
            s_tx_punctured_bits[interleaved_len++] = out_pair[1];
        }
        memcpy(tx->payload_bits, s_tx_punctured_bits, interleaved_len);
        encoded_len = interleaved_len;
    }

    if (tx->punctured && tx->family == PSK_MODE_FAMILY_8PSK) {
        size_t punctured_len = 0U;
        for (size_t i = 0U; i < encoded_len; ++i) {
            if (((i + 1U) & 3U) == 0U) {
                continue;
            }
            if (punctured_len < PSK_TX_BIT_MAX) {
                s_tx_punctured_bits[punctured_len++] = tx->payload_bits[i] ? 1U : 0U;
            }
        }
        memcpy(tx->payload_bits, s_tx_punctured_bits, punctured_len);
        encoded_len = punctured_len;
    }

    return encoded_len;
}

static uint8_t take_payload_bit(psk_tx_t *tx, uint8_t *bit)
{
    if (tx->payload_pos >= tx->payload_len) {
        return 0U;
    }
    *bit = tx->payload_bits[tx->payload_pos++] ? 1U : 0U;
    return 1U;
}

static uint8_t bits_to_symbol(const psk_tx_t *tx, const uint8_t *bits, uint8_t count)
{
    (void)tx;
    uint8_t out = 0U;
    for (uint8_t i = 0U; i < count; ++i) {
        out = (uint8_t)(out | ((bits[i] ? 1U : 0U) << i));
    }
    return out;
}

static uint8_t load_symbol(psk_tx_t *tx, uint8_t *values, uint8_t *count)
{
    *count = 0U;
    if (tx->stage == PSK_TX_STAGE_PREAMBLE) {
        if (tx->preamble_left > 0U) {
            --tx->preamble_left;
            for (uint8_t i = 0U; i < tx->carriers; ++i) {
                values[i] = 0U;
            }
            *count = tx->carriers;
            return 1U;
        }
        tx->stage = PSK_TX_STAGE_TEXT;
    }

    if (tx->stage == PSK_TX_STAGE_TEXT) {
        if (tx->family == PSK_MODE_FAMILY_BPSK && tx->carriers > 1U) {
            for (uint8_t i = 0U; i < tx->carriers; ++i) {
                uint8_t bit = 0U;
                if (!take_payload_bit(tx, &bit)) {
                    tx->stage = PSK_TX_STAGE_POSTAMBLE;
                    return load_symbol(tx, values, count);
                }
                values[i] = bit;
            }
            *count = tx->carriers;
            return 1U;
        }

        const uint8_t bits_needed = tx->bits_per_symbol;
        for (uint8_t i = 0U; i < bits_needed; ++i) {
            uint8_t bit = 0U;
            if (!take_payload_bit(tx, &bit)) {
                tx->stage = PSK_TX_STAGE_POSTAMBLE;
                return load_symbol(tx, values, count);
            }
            values[i] = bit;
        }
        *count = bits_needed;
        return 1U;
    }

    if (tx->stage == PSK_TX_STAGE_POSTAMBLE) {
        if (tx->postamble_left > 0U) {
            --tx->postamble_left;
            if (tx->family == PSK_MODE_FAMILY_8PSK && !tx->fec) {
                values[0] = 0U;
                values[1] = 0U;
                values[2] = 1U;
                *count = tx->bits_per_symbol;
                return 1U;
            } else {
                for (uint8_t i = 0U; i < tx->carriers; ++i) {
                    values[i] = 1U;
                }
                *count = tx->carriers;
                return 1U;
            }
        }
        tx->stage = PSK_TX_STAGE_IDLE;
        tx->active = 0U;
        return 0U;
    }

    return 0U;
}

void psk_tx_init(psk_tx_t *tx, uint32_t sample_rate_hz)
{
    if (!tx) {
        return;
    }
    memset(tx, 0, sizeof(*tx));
    tx->sample_rate_hz = sample_rate_hz;
    tx->tone_hz = 1500;
    tx->symbol_samples = sample_rate_hz / 31U;
    if (tx->symbol_samples == 0U) {
        tx->symbol_samples = 1U;
    }
    tx->amplitude = PSK_TX_DEFAULT_AMPLITUDE;
}

int psk_tx_start_text(psk_tx_t *tx, psk_mode_t mode, int32_t tone_hz, const char *text)
{
    if (!tx || !text || *text == '\0') {
        return -1;
    }

    const psk_mode_info_t *info = psk_mode_info(mode);
    if (!info || info->baud <= 0.0f || info->bits_per_symbol == 0U || info->carriers == 0U) {
        return -2;
    }

    reset_tx(tx);
    strncpy(tx->text, text, sizeof(tx->text) - 1U);
    tx->text_len = strlen(tx->text);
    load_mode(info, tx, tone_hz);

    const size_t payload_len = build_payload_stream(tx);
    if (payload_len == 0U) {
        return -3;
    }
    tx->payload_len = payload_len;
    tx->preamble_symbols = info->dcd_bits ? info->dcd_bits : 32U;
    tx->postamble_symbols = tx->preamble_symbols;
    tx->preamble_left = tx->preamble_symbols;
    tx->postamble_left = tx->postamble_symbols;
    tx->active = 1U;
    tx->stage = PSK_TX_STAGE_PREAMBLE;
    tx->symbol_sample = tx->symbol_samples;
    return 0;
}

size_t psk_tx_generate(psk_tx_t *tx, int16_t *out, size_t capacity)
{
    if (!tx || !out || capacity == 0U || !tx->active) {
        return 0U;
    }

    size_t produced = 0U;
    while (produced < capacity && tx->active) {
        if (tx->symbol_sample >= tx->symbol_samples) {
            uint8_t bits[PSK_TX_CARRIER_MAX] = {0};
            uint8_t count = 0U;
            if (!load_symbol(tx, bits, &count)) {
                break;
            }

            if (tx->family == PSK_MODE_FAMILY_BPSK && tx->carriers > 1U) {
                for (uint8_t car = 0U; car < tx->carriers; ++car) {
                    tx->phase_start[car] = tx->data_phase[car];
                    tx->phase_target[car] = tx->data_phase[car];
                    if (bits[car] == 0U) {
                        tx->phase_target[car] += (float)M_PI;
                    }
                    if (tx->phase_target[car] >= (float)(2.0 * M_PI)) {
                        tx->phase_target[car] -= (float)(2.0 * M_PI);
                    }
                    tx->data_phase[car] = tx->phase_target[car];
                }
            } else {
                const uint8_t symbol = bits_to_symbol(tx, bits, count);
                const float phase_delta = psk_codec_phase_delta((psk_mode_family_t)tx->family,
                                                               tx->fec, symbol);
                tx->phase_start[0] = tx->data_phase[0];
                tx->phase_target[0] = tx->data_phase[0] + phase_delta;
                while (tx->phase_target[0] >= (float)(2.0 * M_PI)) {
                    tx->phase_target[0] -= (float)(2.0 * M_PI);
                }
                tx->data_phase[0] = tx->phase_target[0];
            }
            tx->symbol_sample = 0U;
        }

        const float x = (float)tx->symbol_sample / (float)tx->symbol_samples;
        const float smooth = 0.5f - 0.5f * cosf((float)M_PI * x);
        float sample = 0.0f;
        const uint8_t active = tx->carriers ? tx->carriers : 1U;
        for (uint8_t car = 0U; car < active; ++car) {
            const float start = sinf(tx->carrier_phase[car] + tx->phase_start[car]);
            const float target = sinf(tx->carrier_phase[car] + tx->phase_target[car]);
            sample += start * (1.0f - smooth) + target * smooth;
            tx->carrier_phase[car] += tx->carrier_step[car];
            if (tx->carrier_phase[car] >= (float)(2.0 * M_PI)) {
                tx->carrier_phase[car] -= (float)(2.0 * M_PI);
            }
        }
        const float scaled = sample * (float)tx->amplitude / (float)active;
        out[produced++] = (int16_t)scaled;
        ++tx->symbol_sample;
    }

    return produced;
}

int psk_tx_active(const psk_tx_t *tx)
{
    return tx && tx->active;
}
