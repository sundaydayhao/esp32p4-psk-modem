#include "rx_decoder.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>

#include "common/monitor.h"
#include "ft8/decode.h"
#include "ft8/message.h"

#define CW_UNIT_SAMPLES_20_WPM(sample_rate) ((sample_rate) * 60 / (50 * 20))
#define CW_MIN_UNIT_SAMPLES(sample_rate)    ((sample_rate) * 60 / (50 * 45))
#define CW_MAX_UNIT_SAMPLES(sample_rate)    ((sample_rate) * 60 / (50 * 8))
#define FT_MAX_CANDIDATES 40
#define FT_MIN_SYNC_SCORE 10
#define FT_LDPC_ITERATIONS 25
#define FT_CALL_HASH_SIZE 64
#define MFSK16_SYMBITS 4
#define MFSK16_DEPTH 10
#define MFSK16_VITERBI_STATES 64
#define MFSK16_VITERBI_PATHMEM 256
#define MFSK16_VITERBI_TRACEBACK 45
#define MFSK16_POLY1 0x6d
#define MFSK16_POLY2 0x4f
#define MFSK16_PUNCTURE 128

static const char s_cw_table[128] = {
    [2] = 'E', [3] = 'T',
    [4] = 'I', [5] = 'A', [6] = 'N', [7] = 'M',
    [8] = 'S', [9] = 'U', [10] = 'R', [11] = 'W',
    [12] = 'D', [13] = 'K', [14] = 'G', [15] = 'O',
    [16] = 'H', [17] = 'V', [18] = 'F', [20] = 'L',
    [22] = 'P', [23] = 'J', [24] = 'B', [25] = 'X',
    [26] = 'C', [27] = 'Y', [28] = 'Z', [29] = 'Q',
    [30] = 0, [31] = 0,
    [32] = '5', [33] = '4', [35] = '3', [39] = '2',
    [47] = '1', [63] = '0', [62] = '9', [60] = '8',
    [56] = '7', [48] = '6',
    [85] = '.', [76] = ',', [115] = '?', [106] = '/',
    [97] = '=', [45] = '+', [82] = '"', [72] = '&',
    [94] = ':', [50] = '-', [77] = '(', [109] = ')',
};

static const uint16_t s_mfsk16_varidecode[256] = {
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
    0x6F4, 0x6F8, 0x6FC, 0x700, 0x740, 0x750, 0x754, 0x758
};

static struct {
    char callsign[12];
    uint32_t hash;
} s_call_hash[FT_CALL_HASH_SIZE];

static void emit_bytes(decoder_layer_t *d, const uint8_t *data, size_t len)
{
    if (d->config.emit_cb && data && len > 0) {
        d->config.emit_cb(data, len, d->config.userdata);
    }
}

static void emit_byte(decoder_layer_t *d, uint8_t byte)
{
    emit_bytes(d, &byte, 1);
}

static bool ft_hash_lookup(ftx_callsign_hash_type_t hash_type, uint32_t hash, char *callsign)
{
    uint8_t hash_shift = hash_type == FTX_CALLSIGN_HASH_10_BITS ? 12 :
                         (hash_type == FTX_CALLSIGN_HASH_12_BITS ? 10 : 0);
    uint16_t hash10 = (hash >> (12 - hash_shift)) & 0x03ffU;
    int idx = (hash10 * 23) % FT_CALL_HASH_SIZE;

    for (int probe = 0; probe < FT_CALL_HASH_SIZE; ++probe) {
        if (s_call_hash[idx].callsign[0] == '\0') {
            return false;
        }
        if (((s_call_hash[idx].hash & 0x003fffffU) >> hash_shift) == hash) {
            strcpy(callsign, s_call_hash[idx].callsign);
            return true;
        }
        idx = (idx + 1) % FT_CALL_HASH_SIZE;
    }
    return false;
}

static void ft_hash_save(const char *callsign, uint32_t hash)
{
    uint16_t hash10 = (hash >> 12) & 0x03ffU;
    int idx = (hash10 * 23) % FT_CALL_HASH_SIZE;

    for (int probe = 0; probe < FT_CALL_HASH_SIZE; ++probe) {
        if (s_call_hash[idx].callsign[0] == '\0' ||
            ((s_call_hash[idx].hash & 0x003fffffU) == hash &&
             strcmp(s_call_hash[idx].callsign, callsign) == 0)) {
            strncpy(s_call_hash[idx].callsign, callsign, sizeof(s_call_hash[idx].callsign) - 1);
            s_call_hash[idx].callsign[sizeof(s_call_hash[idx].callsign) - 1] = '\0';
            s_call_hash[idx].hash = hash & 0x003fffffU;
            return;
        }
        idx = (idx + 1) % FT_CALL_HASH_SIZE;
    }
}

static int parity_u32(uint32_t value)
{
    value ^= value >> 16;
    value ^= value >> 8;
    value ^= value >> 4;
    value &= 0x0fU;
    return (0x6996U >> value) & 1U;
}

static uint8_t gray_decode(uint8_t value)
{
    value ^= value >> 1;
    value ^= value >> 2;
    value ^= value >> 4;
    return value;
}

static uint8_t clip_softbit(float value)
{
    if (value < 0.0f) {
        return 0;
    }
    if (value > 255.0f) {
        return 255;
    }
    return (uint8_t)lrintf(value);
}

static int mfsk16_varidec(uint32_t symbol)
{
    for (int i = 0; i < 256; ++i) {
        if (s_mfsk16_varidecode[i] == symbol) {
            return i;
        }
    }
    return -1;
}

static void reset_mfsk16_state(decoder_layer_t *d)
{
    memset(d->mfsk_interleave, MFSK16_PUNCTURE, sizeof(d->mfsk_interleave));
    memset(d->mfsk_symbol_pair, MFSK16_PUNCTURE, sizeof(d->mfsk_symbol_pair));
    memset(d->mfsk_viterbi_metric, 0, sizeof(d->mfsk_viterbi_metric));
    memset(d->mfsk_viterbi_history, 0, sizeof(d->mfsk_viterbi_history));
    memset(d->mfsk_viterbi_sequence, 0, sizeof(d->mfsk_viterbi_sequence));
    d->mfsk_symbol_phase = 0;
    d->mfsk_data_shift = 1;
    d->mfsk_viterbi_ptr = 0;
}

static int mfsk16_conv_output(int encoder_state)
{
    return parity_u32(MFSK16_POLY1 & (uint32_t)encoder_state) |
           (parity_u32(MFSK16_POLY2 & (uint32_t)encoder_state) << 1);
}

static int mfsk16_viterbi_traceback(decoder_layer_t *d)
{
    int best_metric = INT_MIN;
    uint8_t best_state = 0;
    uint8_t p = (uint8_t)(d->mfsk_viterbi_ptr - 1U);

    for (int state = 0; state < MFSK16_VITERBI_STATES; ++state) {
        if (d->mfsk_viterbi_metric[p][state] > best_metric) {
            best_metric = d->mfsk_viterbi_metric[p][state];
            best_state = (uint8_t)state;
        }
    }

    d->mfsk_viterbi_sequence[p] = best_state;
    for (int i = 0; i < MFSK16_VITERBI_TRACEBACK; ++i) {
        const uint8_t prev = (uint8_t)(p - 1U);
        d->mfsk_viterbi_sequence[prev] = d->mfsk_viterbi_history[p][d->mfsk_viterbi_sequence[p]];
        p = prev;
    }

    return d->mfsk_viterbi_sequence[p] & 1U;
}

static int mfsk16_viterbi_decode(decoder_layer_t *d, const uint8_t soft_pair[2])
{
    const uint8_t curr = d->mfsk_viterbi_ptr;
    const uint8_t prev = (uint8_t)(curr - 1U);
    int branch_metric[4];

    branch_metric[0] = (128 - soft_pair[1]) + (128 - soft_pair[0]);
    branch_metric[1] = (128 - soft_pair[1]) + ((int)soft_pair[0] - 128);
    branch_metric[2] = ((int)soft_pair[1] - 128) + (128 - soft_pair[0]);
    branch_metric[3] = ((int)soft_pair[1] - 128) + ((int)soft_pair[0] - 128);

    int row_metric[MFSK16_VITERBI_STATES];
    int row_best = INT_MIN;
    for (int state = 0; state < MFSK16_VITERBI_STATES; ++state) {
        const int s0 = state;
        const int s1 = state + MFSK16_VITERBI_STATES;
        const int p0 = s0 >> 1;
        const int p1 = s1 >> 1;
        const int m0 = d->mfsk_viterbi_metric[prev][p0] + branch_metric[mfsk16_conv_output(s0)];
        const int m1 = d->mfsk_viterbi_metric[prev][p1] + branch_metric[mfsk16_conv_output(s1)];

        if (m0 > m1) {
            row_metric[state] = m0;
            d->mfsk_viterbi_history[curr][state] = (uint8_t)p0;
        } else {
            row_metric[state] = m1;
            d->mfsk_viterbi_history[curr][state] = (uint8_t)p1;
        }
        if (row_metric[state] > row_best) {
            row_best = row_metric[state];
        }
    }

    for (int state = 0; state < MFSK16_VITERBI_STATES; ++state) {
        int normalized = row_metric[state] - row_best;
        if (normalized < INT16_MIN) {
            normalized = INT16_MIN;
        }
        d->mfsk_viterbi_metric[curr][state] = (int16_t)normalized;
    }

    d->mfsk_viterbi_ptr++;
    return mfsk16_viterbi_traceback(d);
}

static void mfsk16_recv_bit(decoder_layer_t *d, int bit)
{
    d->mfsk_data_shift = (d->mfsk_data_shift << 1) | (bit ? 1U : 0U);
    if ((d->mfsk_data_shift & 0x07U) != 0x01U) {
        return;
    }

    const int c = mfsk16_varidec(d->mfsk_data_shift >> 1);
    if (c >= 0) {
        emit_byte(d, (uint8_t)c);
    }
    d->mfsk_data_shift = 1;
}

static void mfsk16_deinterleave(decoder_layer_t *d, uint8_t soft[MFSK16_SYMBITS])
{
    for (int depth = 0; depth < MFSK16_DEPTH; ++depth) {
        uint8_t out[MFSK16_SYMBITS];

        for (int row = 0; row < MFSK16_SYMBITS; ++row) {
            const int row_base = depth * MFSK16_SYMBITS * MFSK16_SYMBITS + row * MFSK16_SYMBITS;
            memmove(&d->mfsk_interleave[row_base],
                    &d->mfsk_interleave[row_base + 1],
                    MFSK16_SYMBITS - 1);
            d->mfsk_interleave[row_base + MFSK16_SYMBITS - 1] = soft[row];
        }

        for (int row = 0; row < MFSK16_SYMBITS; ++row) {
            const int row_base = depth * MFSK16_SYMBITS * MFSK16_SYMBITS + row * MFSK16_SYMBITS;
            out[row] = d->mfsk_interleave[row_base + row];
        }
        memcpy(soft, out, MFSK16_SYMBITS);
    }
}

static void on_psk_rtty_char(char c, void *userdata)
{
    decoder_layer_t *d = (decoder_layer_t *)userdata;
    emit_byte(d, (uint8_t)c);
}

static void reset_symbol_state(decoder_layer_t *d)
{
    d->symbol_samples = 0;
    d->ft_frame_len = 0;
    d->cw_mark = 0;
    d->cw_units = 0;
    d->cw_unit_samples = CW_UNIT_SAMPLES_20_WPM(d->config.sample_rate_hz);
    d->cw_code = 1;
    d->cw_len = 0;
    reset_mfsk16_state(d);
}

static void free_ft_monitor(decoder_layer_t *d)
{
    if (d->ft_monitor) {
        monitor_free((monitor_t *)d->ft_monitor);
        free(d->ft_monitor);
        d->ft_monitor = NULL;
    }
}

static void ensure_ft_monitor(decoder_layer_t *d)
{
    if (d->ft_monitor) {
        return;
    }

    monitor_t *mon = calloc(1, sizeof(*mon));
    if (!mon) {
        return;
    }
    monitor_config_t cfg = {
        .f_min = 200.0f,
        .f_max = 3000.0f,
        .sample_rate = d->config.sample_rate_hz,
        .time_osr = 2,
        .freq_osr = 2,
        .protocol = d->config.mode == MODEM_MODE_FT4 ? FTX_PROTOCOL_FT4 : FTX_PROTOCOL_FT8,
    };
    monitor_init(mon, &cfg);
    d->ft_monitor = mon;
}

static void recreate_backend(decoder_layer_t *d)
{
    free_ft_monitor(d);
    d->profile = demod_profile_for_mode(d->config.mode);
    reset_symbol_state(d);

    if (d->profile->family == DEMOD_FAMILY_PSK) {
        psk_rx_config_t config = {
            .mode = d->config.psk_mode,
            .sample_rate_hz = d->config.sample_rate_hz,
            .rx_tone_hz = d->config.rx_tone_hz,
            .squelch = d->config.squelch,
            .afc_enabled = 1,
            .squelch_enabled = 1,
        };
        psk_rx_init(&d->psk_rx, &config);
        return;
    }

    psk_rtty_config_t config = {
        .mode = PSK_RTTY_MODE_RTTY,
        .sample_rate_hz = d->config.sample_rate_hz,
        .rx_tone_hz = d->config.rx_tone_hz,
        .char_cb = on_psk_rtty_char,
        .userdata = d,
    };
    psk_rtty_rx_init(&d->psk_rtty_rx, &config);
    d->psk_rtty_rx.sq_thresh = d->config.squelch;
}

void decoder_layer_init(decoder_layer_t *d, const decoder_layer_config_t *config)
{
    memset(d, 0, sizeof(*d));
    d->config = *config;
    d->config.rx_tone_hz = demod_clamp_tone(d->config.rx_tone_hz);
    recreate_backend(d);
}

void decoder_layer_set_mode(decoder_layer_t *d, modem_mode_t mode)
{
    if (mode >= MODEM_MODE_COUNT) {
        return;
    }
    d->config.mode = mode;
    if (mode == MODEM_MODE_BPSK31) {
        d->config.psk_mode = PSK_MODE_BPSK31;
    } else if (mode == MODEM_MODE_PSK63) {
        d->config.psk_mode = PSK_MODE_BPSK63;
    }
    recreate_backend(d);
}

void decoder_layer_set_psk_mode(decoder_layer_t *d, psk_mode_t mode)
{
    if ((unsigned)mode >= PSK_MODE_COUNT) {
        return;
    }
    d->config.psk_mode = mode;
    d->config.mode = mode == PSK_MODE_BPSK31 ? MODEM_MODE_BPSK31 : MODEM_MODE_PSK63;
    recreate_backend(d);
}

void decoder_layer_set_tone(decoder_layer_t *d, int tone_hz)
{
    d->config.rx_tone_hz = demod_clamp_tone(tone_hz);
    recreate_backend(d);
}

static void append_symbol(decoder_layer_t *d, const int16_t *samples, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (d->symbol_samples < sizeof(d->symbol_buf) / sizeof(d->symbol_buf[0])) {
            d->symbol_buf[d->symbol_samples++] = samples[i];
        }
    }
}

static int symbol_ready(decoder_layer_t *d)
{
    const uint32_t needed = (uint32_t)lrintf((float)d->config.sample_rate_hz / d->profile->symbol_rate);
    return d->symbol_samples >= needed && needed > 0;
}

static void clear_symbol(decoder_layer_t *d)
{
    d->symbol_samples = 0;
}

static void feed_mfsk16(decoder_layer_t *d, const int16_t *samples, size_t count)
{
    append_symbol(d, samples, count);
    while (symbol_ready(d)) {
        float powers[16];
        float bit_soft[MFSK16_SYMBITS] = {0};
        float sum = 0.0f;
        uint8_t soft[MFSK16_SYMBITS];
        const int first = d->config.rx_tone_hz - ((16 - 1) * d->profile->tone_spacing_hz) / 2;

        for (int tone = 0; tone < 16; ++tone) {
            const int freq = first + tone * d->profile->tone_spacing_hz;
            powers[tone] = sqrtf(demod_goertzel_power(d->symbol_buf,
                                                       d->symbol_samples,
                                                       d->config.sample_rate_hz,
                                                       freq));
            sum += powers[tone];
        }
        if (sum < 1e-9f) {
            sum = 1e-9f;
        }

        for (int tone = 0; tone < 16; ++tone) {
            const uint8_t decoded_tone = gray_decode((uint8_t)tone) & 0x0fU;
            for (int bit = 0; bit < MFSK16_SYMBITS; ++bit) {
                const int mask = 1 << (MFSK16_SYMBITS - bit - 1);
                bit_soft[bit] += (decoded_tone & mask) ? powers[tone] : -powers[tone];
            }
        }

        for (int bit = 0; bit < MFSK16_SYMBITS; ++bit) {
            soft[bit] = clip_softbit(128.0f + (bit_soft[bit] / sum) * 256.0f);
        }

        mfsk16_deinterleave(d, soft);

        for (int bit = 0; bit < MFSK16_SYMBITS; ++bit) {
            d->mfsk_symbol_pair[0] = d->mfsk_symbol_pair[1];
            d->mfsk_symbol_pair[1] = soft[bit];
            d->mfsk_symbol_phase ^= 1U;
            if (d->mfsk_symbol_phase == 0) {
                const int decoded_bit = mfsk16_viterbi_decode(d, d->mfsk_symbol_pair);
                mfsk16_recv_bit(d, decoded_bit);
            }
        }

        clear_symbol(d);
    }
}

static void decode_ft_window(decoder_layer_t *d)
{
    monitor_t *mon = (monitor_t *)d->ft_monitor;
    if (!mon || mon->wf.num_blocks < mon->wf.max_blocks) {
        return;
    }

    ftx_candidate_t candidates[FT_MAX_CANDIDATES];
    const int num_candidates = ftx_find_candidates(&mon->wf, FT_MAX_CANDIDATES, candidates, FT_MIN_SYNC_SCORE);
    ftx_callsign_hash_interface_t hash_if = {
        .lookup_hash = ft_hash_lookup,
        .save_hash = ft_hash_save,
    };

    ftx_message_t decoded[8];
    int decoded_count = 0;
    for (int i = 0; i < num_candidates; ++i) {
        ftx_message_t message;
        ftx_decode_status_t status;
        if (!ftx_decode_candidate(&mon->wf, &candidates[i], FT_LDPC_ITERATIONS, &message, &status)) {
            continue;
        }

        int duplicate = 0;
        for (int j = 0; j < decoded_count; ++j) {
            if (decoded[j].hash == message.hash &&
                memcmp(decoded[j].payload, message.payload, sizeof(message.payload)) == 0) {
                duplicate = 1;
                break;
            }
        }
        if (duplicate) {
            continue;
        }

        if (decoded_count < (int)(sizeof(decoded) / sizeof(decoded[0]))) {
            decoded[decoded_count++] = message;
        }

        char text[FTX_MAX_MESSAGE_LENGTH + 4];
        ftx_message_offsets_t offsets;
        if (ftx_message_decode(&message, &hash_if, text, &offsets) == FTX_MESSAGE_RC_OK) {
            size_t len = strlen(text);
            if (len + 2 < sizeof(text)) {
                text[len++] = '\n';
                text[len] = '\0';
            }
            emit_bytes(d, (const uint8_t *)text, len);
        }
    }

    monitor_reset(mon);
}

static void feed_ft_symbols(decoder_layer_t *d, const int16_t *samples, size_t count)
{
    ensure_ft_monitor(d);
    monitor_t *mon = (monitor_t *)d->ft_monitor;
    if (!mon) {
        return;
    }

    for (size_t i = 0; i < count; ++i) {
        if (d->ft_frame_len < sizeof(d->ft_frame) / sizeof(d->ft_frame[0])) {
            d->ft_frame[d->ft_frame_len++] = (float)samples[i] / 32768.0f;
        }
        if (d->ft_frame_len >= (size_t)mon->block_size) {
            monitor_process(mon, d->ft_frame);
            d->ft_frame_len = 0;
            decode_ft_window(d);
        }
    }
}

static void update_cw_unit(decoder_layer_t *d, int mark_units)
{
    if (mark_units <= 0) {
        return;
    }
    uint32_t estimate = d->cw_unit_samples;
    if (mark_units >= 2) {
        estimate = (d->cw_unit_samples * (uint32_t)mark_units) / 3U;
    }
    if (estimate < CW_MIN_UNIT_SAMPLES(d->config.sample_rate_hz)) {
        estimate = CW_MIN_UNIT_SAMPLES(d->config.sample_rate_hz);
    } else if (estimate > CW_MAX_UNIT_SAMPLES(d->config.sample_rate_hz)) {
        estimate = CW_MAX_UNIT_SAMPLES(d->config.sample_rate_hz);
    }
    d->cw_unit_samples = (d->cw_unit_samples * 3U + estimate) / 4U;
}

static void finish_cw_element(decoder_layer_t *d, int mark, int units)
{
    if (mark) {
        update_cw_unit(d, units);
        d->cw_code <<= 1;
        if (units >= 3) {
            d->cw_code |= 1;
        }
        if (d->cw_len < 5) {
            d->cw_len++;
        }
        return;
    }

    if (units >= 7) {
        if (d->cw_code < sizeof(s_cw_table) && s_cw_table[d->cw_code]) {
            emit_byte(d, (uint8_t)s_cw_table[d->cw_code]);
        }
        emit_byte(d, ' ');
        d->cw_code = 1;
        d->cw_len = 0;
    } else if (units >= 3) {
        if (d->cw_code < sizeof(s_cw_table) && s_cw_table[d->cw_code]) {
            emit_byte(d, (uint8_t)s_cw_table[d->cw_code]);
        }
        d->cw_code = 1;
        d->cw_len = 0;
    }
}

static void feed_cw(decoder_layer_t *d, const int16_t *samples, size_t count)
{
    const uint32_t unit_samples = d->cw_unit_samples;
    append_symbol(d, samples, count);
    while (d->symbol_samples >= unit_samples && unit_samples > 0) {
        const float tone_power = demod_goertzel_power(d->symbol_buf, unit_samples, d->config.sample_rate_hz, d->config.rx_tone_hz);
        const float side_power = demod_goertzel_power(d->symbol_buf, unit_samples, d->config.sample_rate_hz, d->config.rx_tone_hz + 120);
        const int mark = tone_power > side_power * 2.5f && tone_power > 0.0008f;

        if (d->cw_units == 0) {
            d->cw_mark = mark;
            d->cw_units = 1;
        } else if (mark == d->cw_mark) {
            d->cw_units++;
        } else {
            finish_cw_element(d, d->cw_mark, d->cw_units);
            d->cw_mark = mark;
            d->cw_units = 1;
        }
        clear_symbol(d);
    }
}

void decoder_layer_feed(decoder_layer_t *d, const int16_t *samples, size_t count)
{
    if (!samples || count == 0) {
        return;
    }

    switch (d->profile->family) {
    case DEMOD_FAMILY_PSK: {
        char decoded[96];
        const size_t decoded_len = psk_rx_process(&d->psk_rx, samples, count,
                                                  decoded, sizeof(decoded));
        emit_bytes(d, (const uint8_t *)decoded, decoded_len);
        break;
    }
    case DEMOD_FAMILY_FSK:
        psk_rtty_rx_feed(&d->psk_rtty_rx, samples, count);
        break;
    case DEMOD_FAMILY_CW:
        feed_cw(d, samples, count);
        break;
    case DEMOD_FAMILY_MFSK:
        feed_mfsk16(d, samples, count);
        break;
    case DEMOD_FAMILY_FT:
        feed_ft_symbols(d, samples, count);
        break;
    default:
        break;
    }
}
