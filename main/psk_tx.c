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

#define PSK_TX_DEFAULT_AMPLITUDE 26000
#define PSK_TX_PEAK_LIMIT        30000.0f

static uint16_t fec_8psk_flush_chars(psk_mode_t mode)
{
    switch (mode) {
    case PSK_MODE_8PSK125F:
        return 55U;
    case PSK_MODE_8PSK250F:
        return 65U;
    case PSK_MODE_8PSK500F:
        return 80U;
    case PSK_MODE_8PSK1000F:
        return 120U;
    case PSK_MODE_8PSK1200F:
    case PSK_MODE_2X8PSK1200F:
        return 175U;
    default:
        return 0U;
    }
}

static float wrap_phase(float phase)
{
    while (phase >= (float)(2.0 * M_PI)) {
        phase -= (float)(2.0 * M_PI);
    }
    while (phase < 0.0f) {
        phase += (float)(2.0 * M_PI);
    }
    return phase;
}

static uint32_t highest_bit_mask(uint32_t value)
{
    uint32_t mask = 1UL << 31;
    while (mask != 0U && (value & mask) == 0U) {
        mask >>= 1U;
    }
    return mask;
}

static void set_symbol_transition(psk_tx_t *tx, uint8_t car, float target_phase)
{
    const float start_phase = tx->data_phase[car];
    target_phase = wrap_phase(target_phase);

    tx->phase_start[car] = start_phase;
    tx->phase_target[car] = target_phase;
    tx->symbol_start_i[car] = cosf(start_phase);
    tx->symbol_start_q[car] = sinf(start_phase);
    tx->symbol_target_i[car] = cosf(target_phase);
    tx->symbol_target_q[car] = sinf(target_phase);
    tx->data_phase[car] = target_phase;
}

static void reset_tx(psk_tx_t *tx)
{
    memset(tx->payload_bits, 0, sizeof(tx->payload_bits));
    memset(tx->symbol_bits, 0, sizeof(tx->symbol_bits));
    memset(tx->carrier_phase, 0, sizeof(tx->carrier_phase));
    memset(tx->carrier_step, 0, sizeof(tx->carrier_step));
    memset(tx->data_phase, 0, sizeof(tx->data_phase));
    memset(tx->phase_start, 0, sizeof(tx->phase_start));
    memset(tx->phase_target, 0, sizeof(tx->phase_target));
    memset(tx->symbol_start_i, 0, sizeof(tx->symbol_start_i));
    memset(tx->symbol_start_q, 0, sizeof(tx->symbol_start_q));
    memset(tx->symbol_target_i, 0, sizeof(tx->symbol_target_i));
    memset(tx->symbol_target_q, 0, sizeof(tx->symbol_target_q));
    tx->symbol_position = 0.0f;
    tx->payload_len = 0U;
    tx->payload_pos = 0U;
    tx->active = 0U;
    tx->stage = PSK_TX_STAGE_IDLE;
    tx->repeat_payload = 0U;
    tx->fec_flush_chars = 0U;
    tx->fec_preamble_bits = 0U;
    tx->fec_stream = 0U;
    tx->fec_stream_out_len = 0U;
    tx->fec_stream_out_pos = 0U;
    tx->fec_stream_puncture_phase = 0U;
    tx->fec_stream_warmup_bits_left = 0U;
    tx->fec_stream_state = 0U;
    tx->stream_fetch = NULL;
    tx->stream_user = NULL;
    tx->stream_code = 0U;
    tx->stream_mask = 0U;
    tx->binary_stream = 0U;
}

static void load_mode(const psk_mode_info_t *info, psk_tx_t *tx, int32_t tone_hz)
{
    tx->family = (uint8_t)info->family;
    tx->carriers = info->carriers ? info->carriers : 1U;
    tx->bits_per_symbol = info->bits_per_symbol ? info->bits_per_symbol : 1U;
    tx->fec = info->fec;
    tx->punctured = info->punctured;
    tx->mfsk_varicode = info->mfsk_varicode;
    tx->dual_fec_branch = info->dual_fec_branch;
    tx->fec_k = info->fec_k;
    tx->interleave_depth = info->interleave_depth;
    tx->fec_poly1 = info->fec_poly1;
    tx->fec_poly2 = info->fec_poly2;
    tx->fec_flush_chars = fec_8psk_flush_chars(info->id);
    tx->fec_preamble_bits = 0U;
    tx->tone_hz = tone_hz > 0 ? tone_hz : PSK_DEFAULT_TONE_HZ;
    tx->symbol_period_samples = (float)tx->sample_rate_hz / info->baud;
    tx->symbol_samples = (uint32_t)(tx->symbol_period_samples + 0.5f);
    if (tx->symbol_samples == 0U) {
        tx->symbol_samples = 1U;
    }
    tx->amplitude_scale = (float)tx->amplitude;
    if (tx->carriers > 1U) {
        /* fldigi normalizes each multi-carrier symbol block after summing
         * carriers. Use RMS-preserving scaling here: it keeps worst-case peaks
         * below int16 while avoiding a 1/N per-carrier SNR loss on the board
         * audio path. */
        const float carriers = (float)tx->carriers;
        tx->amplitude_scale /= sqrtf(carriers);
        const float peak_limited_scale = PSK_TX_PEAK_LIMIT / carriers;
        if (tx->amplitude_scale > peak_limited_scale) {
            tx->amplitude_scale = peak_limited_scale;
        }

        /* PSK125C12 is a text mode.  Its twelve carriers were previously
         * limited to 1/12 of the full-scale peak, which leaves too little
         * per-carrier SNR after the WM8960 line path.  Keep the voice bridge
         * modes unchanged and give this text mode modest extra headroom. */
        if (info->id == PSK_MODE_12XPSK125) {
            const float text_peak_scale = 3500.0f;
            if (tx->amplitude_scale < text_peak_scale) {
                tx->amplitude_scale = text_peak_scale;
            }
        }
    }

    const float spacing = 1.4f * info->baud;
    const float first = (float)tx->tone_hz + (((-1.0f * (float)tx->carriers) + 1.0f) * spacing * 0.5f);
    for (uint8_t car = 0U; car < tx->carriers; ++car) {
        const float freq = first + (float)car * spacing;
        tx->carrier_step[car] = (float)(2.0 * M_PI * (double)freq / (double)tx->sample_rate_hz);
        /* A zero-phase multitone produces a large coherent peak at startup.
         * Newman quadratic phases lower the crest factor without changing
         * any differential data phase, carrier frequency, or decoder rule. */
        tx->carrier_phase[car] = tx->carriers > 1U ?
            wrap_phase((float)M_PI * (float)car * (float)car / (float)tx->carriers) : 0.0f;
        tx->data_phase[car] = 0.0f;
        tx->phase_start[car] = 0.0f;
        tx->phase_target[car] = 0.0f;
        tx->symbol_start_i[car] = 1.0f;
        tx->symbol_start_q[car] = 0.0f;
        tx->symbol_target_i[car] = 1.0f;
        tx->symbol_target_q[car] = 0.0f;
    }
}

static size_t build_raw_payload_stream(psk_tx_t *tx, const char *text,
                                       uint8_t append_fec_preamble,
                                       uint8_t append_fec_flush)
{
    size_t raw_len = 0U;
    if (append_fec_preamble) {
        for (uint16_t i = 0U; i < tx->fec_preamble_bits; ++i) {
            const uint8_t bit = (tx->family == PSK_MODE_FAMILY_BPSK && tx->mfsk_varicode) ?
                (uint8_t)((i & 1U) == 0U) : 0U;
            psk_codec_append_bit(s_tx_raw_bits, &raw_len, bit);
        }
    }
    if (tx->mfsk_varicode) {
        if (tx->family == PSK_MODE_FAMILY_8PSK ||
            (tx->family == PSK_MODE_FAMILY_BPSK && tx->fec)) {
            psk_codec_append_mfsk_char(s_tx_raw_bits, &raw_len, 0U);
        }
        psk_codec_append_mfsk_text(s_tx_raw_bits, &raw_len, text);
        if (append_fec_flush && tx->family == PSK_MODE_FAMILY_8PSK && tx->fec) {
            const uint16_t flush_chars = tx->fec_flush_chars ? tx->fec_flush_chars : 32U;
            for (uint16_t i = 0U; i < flush_chars; ++i) {
                psk_codec_append_mfsk_char(s_tx_raw_bits, &raw_len, 0U);
            }
        } else if (append_fec_flush && tx->family == PSK_MODE_FAMILY_BPSK && tx->fec) {
            /* fldigi::tx_flush sends dcdbits/2 pairs of input 1,1 for
             * PSK63F, allowing the decoder traceback to drain. */
            for (uint16_t i = 0U; i < tx->fec_preamble_bits; ++i) {
                psk_codec_append_bit(s_tx_raw_bits, &raw_len, 1U);
            }
        }
    } else {
        psk_codec_append_varicode_text(s_tx_raw_bits, &raw_len, text);
        if (append_fec_flush && tx->family == PSK_MODE_FAMILY_QPSK) {
            for (uint16_t i = 0U; i < tx->preamble_symbols; ++i) {
                psk_codec_append_bit(s_tx_raw_bits, &raw_len, 0U);
            }
        }
    }

    return raw_len;
}

static size_t build_encoded_payload_stream(psk_tx_t *tx, size_t raw_len)
{
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

static size_t build_payload_stream(psk_tx_t *tx, uint8_t repeat_payload)
{
    const char *text = tx->text;

    const uint8_t append_fec_preamble = (uint8_t)(tx->fec_preamble_bits != 0U && !repeat_payload);
    const size_t raw_len = build_raw_payload_stream(tx, text, append_fec_preamble,
                                                    (uint8_t)(tx->fec && !repeat_payload));
    if (raw_len == 0U) {
        return 0U;
    }

    if (tx->fec && repeat_payload) {
        memcpy(tx->payload_bits, s_tx_raw_bits, raw_len);
        tx->fec_stream = 1U;
        tx->fec_stream_state = 0U;
        tx->fec_stream_out_len = 0U;
        tx->fec_stream_out_pos = 0U;
        tx->fec_stream_puncture_phase = 0U;
        tx->fec_stream_warmup_bits_left = tx->fec_preamble_bits;
        psk_codec_interleave2_reset(s_tx_interleave_table, sizeof(s_tx_interleave_table));
        return raw_len;
    }

    return build_encoded_payload_stream(tx, raw_len);
}

static uint8_t take_raw_payload_bit(psk_tx_t *tx, uint8_t *bit)
{
    if (tx->fec_stream_warmup_bits_left > 0U) {
        /* fldigi's BPSK63F preamble is 32 repetitions of input bits 1,0
         * before the MFSK NUL marker.  The 8PSK FEC modes retain their
         * existing zero warmup sequence. */
        if (tx->family == PSK_MODE_FAMILY_BPSK && tx->mfsk_varicode) {
            *bit = (tx->fec_stream_warmup_bits_left & 1U) == 0U ? 1U : 0U;
        } else {
            *bit = 0U;
        }
        --tx->fec_stream_warmup_bits_left;
        return 1U;
    }
    if (tx->stream_fetch) {
        if (tx->binary_stream) {
            if (tx->stream_mask == 0U) {
                int ch = tx->stream_fetch(tx->stream_user);
                if (ch < 0) {
                    ch = 0;
                }
                tx->stream_code = (uint32_t)(uint8_t)ch;
                tx->stream_mask = 0x80U;
            }
            *bit = (tx->stream_code & tx->stream_mask) ? 1U : 0U;
            tx->stream_mask >>= 1U;
            return 1U;
        }
        while (tx->stream_mask == 0U) {
            int ch = tx->stream_fetch(tx->stream_user);
            if (ch < 0) {
                ch = 'X';
            }
            if (tx->mfsk_varicode) {
                tx->stream_code = psk_codec_mfsk_varicode_table()[(uint8_t)ch];
            } else {
                /* Standard PSK Varicode uses two zero bits as the character
                 * separator. Appending them to the code makes continuous
                 * callback streams behave exactly like buffered text. */
                tx->stream_code = psk_codec_varicode_code((uint8_t)ch) << 2U;
            }
            tx->stream_mask = highest_bit_mask(tx->stream_code);
        }
        *bit = (tx->stream_code & tx->stream_mask) ? 1U : 0U;
        tx->stream_mask >>= 1U;
        return 1U;
    }
    if (tx->payload_pos >= tx->payload_len) {
        if (!tx->repeat_payload || tx->payload_len == 0U) {
            return 0U;
        }
        tx->payload_pos = 0U;
    }
    *bit = tx->payload_bits[tx->payload_pos++] ? 1U : 0U;
    return 1U;
}

static uint8_t queue_fec_stream_bit(psk_tx_t *tx, uint8_t bit)
{
    if (tx->punctured && tx->family == PSK_MODE_FAMILY_8PSK) {
        tx->fec_stream_puncture_phase = (uint8_t)((tx->fec_stream_puncture_phase + 1U) & 3U);
        if (tx->fec_stream_puncture_phase == 0U) {
            return 1U;
        }
    }
    if (tx->fec_stream_out_len >= sizeof(tx->fec_stream_out)) {
        return 0U;
    }
    tx->fec_stream_out[tx->fec_stream_out_len++] = bit ? 1U : 0U;
    return 1U;
}

static uint8_t fill_fec_stream_queue(psk_tx_t *tx)
{
    uint8_t raw_bit = 0U;
    if (!take_raw_payload_bit(tx, &raw_bit)) {
        return 0U;
    }

    uint8_t encoded_pair[2];
    if (!psk_codec_conv_encode_bit(&tx->fec_stream_state, raw_bit, tx->fec_k,
                                   tx->fec_poly1, tx->fec_poly2, encoded_pair)) {
        return 0U;
    }
    if (tx->interleave_depth != 0U) {
        uint8_t interleaved_pair[2];
        if (!psk_codec_interleave2_pair(s_tx_interleave_table, sizeof(s_tx_interleave_table),
                                        tx->interleave_depth,
                                        encoded_pair[0], encoded_pair[1], interleaved_pair)) {
            return 0U;
        }
        encoded_pair[0] = interleaved_pair[0];
        encoded_pair[1] = interleaved_pair[1];
    }

    return queue_fec_stream_bit(tx, encoded_pair[0]) &&
           queue_fec_stream_bit(tx, encoded_pair[1]);
}

static uint8_t take_fec_stream_bit(psk_tx_t *tx, uint8_t *bit)
{
    while (tx->fec_stream_out_pos >= tx->fec_stream_out_len) {
        tx->fec_stream_out_pos = 0U;
        tx->fec_stream_out_len = 0U;
        if (!fill_fec_stream_queue(tx)) {
            return 0U;
        }
    }

    *bit = tx->fec_stream_out[tx->fec_stream_out_pos++] ? 1U : 0U;
    return 1U;
}

static uint8_t take_payload_bit(psk_tx_t *tx, uint8_t *bit)
{
    if (tx->fec_stream) {
        return take_fec_stream_bit(tx, bit);
    }
    if (tx->stream_fetch) {
        return take_raw_payload_bit(tx, bit);
    }
    if (tx->payload_pos >= tx->payload_len) {
        if (!tx->repeat_payload || tx->payload_len == 0U) {
            return 0U;
        }
        tx->payload_pos = 0U;
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
            if (tx->fec && tx->family == PSK_MODE_FAMILY_8PSK) {
                /* fldigi disables FEC for the first half of the 8PSK FEC
                 * preamble and sends non-FEC symbol 0. With this Gray map,
                 * symbol 6 produces the same 180 degree differential phase. */
                for (uint8_t car = 0U; car < tx->carriers; ++car) {
                    values[car * tx->bits_per_symbol + 0U] = 0U;
                    values[car * tx->bits_per_symbol + 1U] = 1U;
                    values[car * tx->bits_per_symbol + 2U] = 1U;
                }
                *count = (uint8_t)(tx->bits_per_symbol * tx->carriers);
                return 1U;
            }
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
        if (tx->family == PSK_MODE_FAMILY_8PSK && tx->carriers > 1U) {
            for (uint8_t car = 0U; car < tx->carriers; ++car) {
                for (uint8_t bit_idx = 0U; bit_idx < tx->bits_per_symbol; ++bit_idx) {
                    uint8_t bit = 0U;
                    if (!take_payload_bit(tx, &bit)) {
                        tx->stage = PSK_TX_STAGE_POSTAMBLE;
                        return load_symbol(tx, values, count);
                    }
                    values[(size_t)car * tx->bits_per_symbol + bit_idx] = bit;
                }
            }
            *count = (uint8_t)(tx->bits_per_symbol * tx->carriers);
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
            if (tx->family == PSK_MODE_FAMILY_8PSK) {
                const uint8_t idle_symbol = tx->fec ? 0U : 4U;
                for (uint8_t car = 0U; car < tx->carriers; ++car) {
                    for (uint8_t i = 0U; i < tx->bits_per_symbol; ++i) {
                        values[(size_t)car * tx->bits_per_symbol + i] =
                            (uint8_t)((idle_symbol >> i) & 1U);
                    }
                }
                *count = (uint8_t)(tx->bits_per_symbol * tx->carriers);
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
    tx->tone_hz = PSK_DEFAULT_TONE_HZ;
    tx->symbol_samples = sample_rate_hz / 31U;
    if (tx->symbol_samples == 0U) {
        tx->symbol_samples = 1U;
    }
    tx->symbol_period_samples = (float)tx->symbol_samples;
    tx->symbol_position = 0.0f;
    tx->amplitude = PSK_TX_DEFAULT_AMPLITUDE;
}

static int start_text(psk_tx_t *tx, psk_mode_t mode, int32_t tone_hz,
                      const char *text, uint8_t repeat_payload)
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
    tx->preamble_symbols = info->dcd_bits ? info->dcd_bits : 32U;
    if (tx->fec && tx->family == PSK_MODE_FAMILY_8PSK) {
        tx->fec_preamble_bits = tx->preamble_symbols;
        tx->preamble_symbols /= 2U;
    } else if (tx->fec && tx->family == PSK_MODE_FAMILY_BPSK && tx->mfsk_varicode) {
        tx->fec_preamble_bits = tx->preamble_symbols;
        tx->preamble_symbols = 0U;
    }
    tx->postamble_symbols = (tx->fec && tx->family == PSK_MODE_FAMILY_8PSK) ?
        0U : tx->preamble_symbols;
    tx->repeat_payload = repeat_payload ? 1U : 0U;

    const size_t payload_len = build_payload_stream(tx, repeat_payload);
    if (payload_len == 0U) {
        return -3;
    }
    tx->payload_len = payload_len;
    tx->payload_pos = 0U;
    tx->preamble_left = tx->preamble_symbols;
    tx->postamble_left = tx->postamble_symbols;
    tx->stage = PSK_TX_STAGE_PREAMBLE;
    tx->active = 1U;
    tx->symbol_position = tx->symbol_period_samples;
    return 0;
}

int psk_tx_start_text(psk_tx_t *tx, psk_mode_t mode, int32_t tone_hz, const char *text)
{
    return start_text(tx, mode, tone_hz, text, 0U);
}

int psk_tx_start_text_loop(psk_tx_t *tx, psk_mode_t mode, int32_t tone_hz,
                           const char *text)
{
    return start_text(tx, mode, tone_hz, text, 1U);
}

int psk_tx_start_stream(psk_tx_t *tx, psk_mode_t mode, int32_t tone_hz,
                        psk_tx_stream_fetch_cb_t fetch, void *user)
{
    if (!tx || !fetch) {
        return -1;
    }

    const psk_mode_info_t *info = psk_mode_info(mode);
    if (!info || info->baud <= 0.0f || info->bits_per_symbol == 0U || info->carriers == 0U) {
        return -2;
    }

    reset_tx(tx);
    load_mode(info, tx, tone_hz);
    tx->preamble_symbols = info->dcd_bits ? info->dcd_bits : 32U;
    if (tx->fec && tx->family == PSK_MODE_FAMILY_8PSK) {
        tx->fec_preamble_bits = tx->preamble_symbols;
        tx->preamble_symbols /= 2U;
    } else if (tx->fec && tx->family == PSK_MODE_FAMILY_BPSK && tx->mfsk_varicode) {
        tx->fec_preamble_bits = tx->preamble_symbols;
        tx->preamble_symbols = 0U;
    }
    tx->postamble_symbols = 0U;
    tx->repeat_payload = 1U;
    tx->fec_stream = tx->fec ? 1U : 0U;
    tx->fec_stream_state = 0U;
    tx->fec_stream_out_len = 0U;
    tx->fec_stream_out_pos = 0U;
    tx->fec_stream_puncture_phase = 0U;
    tx->fec_stream_warmup_bits_left = tx->fec_preamble_bits;
    tx->stream_fetch = fetch;
    tx->stream_user = user;
    tx->stream_code = 0U;
    tx->stream_mask = 0U;
    if (tx->mfsk_varicode) {
        tx->stream_code = psk_codec_mfsk_varicode_table()[0U];
        tx->stream_mask = highest_bit_mask(tx->stream_code);
    }
    psk_codec_interleave2_reset(s_tx_interleave_table, sizeof(s_tx_interleave_table));
    tx->preamble_left = tx->preamble_symbols;
    tx->postamble_left = tx->postamble_symbols;
    tx->stage = PSK_TX_STAGE_PREAMBLE;
    tx->active = 1U;
    tx->symbol_position = tx->symbol_period_samples;
    return 0;
}

int psk_tx_start_binary_stream(psk_tx_t *tx, psk_mode_t mode, int32_t tone_hz,
                               psk_tx_stream_fetch_cb_t fetch, void *user)
{
    const int rc = psk_tx_start_stream(tx, mode, tone_hz, fetch, user);
    if (rc == 0) {
        tx->binary_stream = 1U;
        /* Raw bytes must not inherit the text Varicode start marker. */
        tx->stream_code = 0U;
        tx->stream_mask = 0U;
    }
    return rc;
}

int psk_tx_start_binary_data(psk_tx_t *tx, psk_mode_t mode, int32_t tone_hz,
                             const uint8_t *data, size_t length)
{
    if (!tx || !data || length == 0U) {
        return -1;
    }

    const psk_mode_info_t *info = psk_mode_info(mode);
    if (!info || info->baud <= 0.0f || info->bits_per_symbol == 0U || info->carriers == 0U) {
        return -2;
    }

    reset_tx(tx);
    load_mode(info, tx, tone_hz);
    tx->preamble_symbols = info->dcd_bits ? info->dcd_bits : 32U;
    if (tx->fec && tx->family == PSK_MODE_FAMILY_8PSK) {
        tx->fec_preamble_bits = tx->preamble_symbols;
        tx->preamble_symbols /= 2U;
    }
    tx->postamble_symbols = 0U;

    size_t raw_len = 0U;
    for (uint16_t i = 0U; i < tx->fec_preamble_bits; ++i) {
        psk_codec_append_bit(s_tx_raw_bits, &raw_len, 0U);
    }
    for (size_t pos = 0U; pos < length; ++pos) {
        for (int bit = 7; bit >= 0; --bit) {
            psk_codec_append_bit(s_tx_raw_bits, &raw_len,
                                 (uint8_t)((data[pos] >> bit) & 1U));
        }
    }
    if (tx->fec && tx->family == PSK_MODE_FAMILY_8PSK) {
        const uint16_t flush_chars = tx->fec_flush_chars ? tx->fec_flush_chars : 32U;
        for (uint16_t i = 0U; i < (uint16_t)(flush_chars * 8U); ++i) {
            psk_codec_append_bit(s_tx_raw_bits, &raw_len, 0U);
        }
    }

    const size_t payload_len = build_encoded_payload_stream(tx, raw_len);
    if (payload_len == 0U) {
        return -3;
    }

    tx->payload_len = payload_len;
    tx->payload_pos = 0U;
    tx->preamble_left = tx->preamble_symbols;
    tx->postamble_left = tx->postamble_symbols;
    tx->stage = PSK_TX_STAGE_PREAMBLE;
    tx->active = 1U;
    tx->symbol_position = tx->symbol_period_samples;
    tx->binary_stream = 1U;
    return 0;
}

size_t psk_tx_generate(psk_tx_t *tx, int16_t *out, size_t capacity)
{
    if (!tx || !out || capacity == 0U || !tx->active) {
        return 0U;
    }

    size_t produced = 0U;
    while (produced < capacity && tx->active) {
        if (tx->symbol_position >= tx->symbol_period_samples) {
            uint8_t bits[PSK_TX_CARRIER_MAX] = {0};
            uint8_t count = 0U;
            if (!load_symbol(tx, bits, &count)) {
                break;
            }
            tx->symbol_position -= tx->symbol_period_samples;

        if (tx->family == PSK_MODE_FAMILY_BPSK && tx->carriers > 1U) {
            for (uint8_t car = 0U; car < tx->carriers; ++car) {
                float target_phase = tx->data_phase[car];
                if (bits[car] == 0U) {
                    target_phase += (float)M_PI;
                }
                set_symbol_transition(tx, car, target_phase);
            }
        } else if (tx->family == PSK_MODE_FAMILY_8PSK && tx->carriers > 1U) {
            for (uint8_t car = 0U; car < tx->carriers; ++car) {
                const uint8_t symbol = bits_to_symbol(tx,
                                                       &bits[(size_t)car * tx->bits_per_symbol],
                                                       tx->bits_per_symbol);
                const float phase_delta = psk_codec_phase_delta((psk_mode_family_t)tx->family,
                                                                tx->fec, symbol);
                set_symbol_transition(tx, car, tx->data_phase[car] + phase_delta);
            }
        } else {
            const uint8_t symbol = bits_to_symbol(tx, bits, count);
            const float phase_delta = psk_codec_phase_delta((psk_mode_family_t)tx->family,
                                                           tx->fec, symbol);
            set_symbol_transition(tx, 0U, tx->data_phase[0] + phase_delta);
            }
        }

        const float x = tx->symbol_period_samples > 0.0f ?
            tx->symbol_position / tx->symbol_period_samples : 0.0f;
        const float shape_b = 0.5f - 0.5f * cosf((float)M_PI * x);
        const float shape_a = 1.0f - shape_b;
        float sample = 0.0f;
        const uint8_t active = tx->carriers ? tx->carriers : 1U;
        for (uint8_t car = 0U; car < active; ++car) {
            const float i = shape_a * tx->symbol_start_i[car] +
                            shape_b * tx->symbol_target_i[car];
            const float q = shape_a * tx->symbol_start_q[car] +
                            shape_b * tx->symbol_target_q[car];
            const float carrier_cos = cosf(tx->carrier_phase[car]);
            const float carrier_sin = sinf(tx->carrier_phase[car]);
            sample += i * carrier_cos + q * carrier_sin;
            tx->carrier_phase[car] += tx->carrier_step[car];
            if (tx->carrier_phase[car] >= (float)(2.0 * M_PI)) {
                tx->carrier_phase[car] -= (float)(2.0 * M_PI);
            }
        }
        float scaled = sample * tx->amplitude_scale;
        if (scaled > 32767.0f) {
            scaled = 32767.0f;
        } else if (scaled < -32768.0f) {
            scaled = -32768.0f;
        }
        out[produced++] = (int16_t)scaled;
        tx->symbol_position += 1.0f;
    }

    return produced;
}

int psk_tx_active(const psk_tx_t *tx)
{
    return tx && tx->active;
}

void psk_tx_stop(psk_tx_t *tx)
{
    if (tx) {
        reset_tx(tx);
    }
}
