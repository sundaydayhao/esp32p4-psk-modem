#include "psk_rx.h"

#include <string.h>

#define MODEM_TWO_PI 6.28318530717958647692f
static psk_rtty_mode_t to_backend_mode(psk_mode_t mode)
{
    const psk_mode_info_t *info = psk_mode_info(mode);
    return info->backend_mode;
}

static void emit_char(psk_rx_t *m, char c)
{
    if (!m->out || m->out_len + 1 >= m->out_size) {
        return;
    }
    m->out[m->out_len++] = c;
    m->out[m->out_len] = '\0';
}

static int is_plaintext_char(char c)
{
    return c == '\r' || c == '\n' || c == '\t' || (c >= 32 && c <= 126);
}

static void frame1200f_reset(psk1200f_frame_t *frame)
{
    frame->state = PSK1200F_FRAME_SEARCH;
    frame->header_len = 0U;
    frame->base64_len = 0U;
    frame->expected_len = 0U;
    frame->expected_crc = 0U;
}

static void frame1200f_emit_buffered_header(psk_rx_t *m, char suffix)
{
    psk1200f_frame_t *frame = &m->frame1200f;
    emit_char(m, '~');
    for (uint16_t i = 0U; i < frame->header_len; ++i) {
        emit_char(m, frame->header[i]);
    }
    if (suffix != '\0' && is_plaintext_char(suffix)) {
        emit_char(m, suffix);
    }
    frame1200f_reset(frame);
}

static int parse_decimal(const char **cursor, uint32_t *value)
{
    const char *p = *cursor;
    uint32_t out = 0U;
    uint8_t count = 0U;
    while (*p >= '0' && *p <= '9') {
        if (out > 429496729U) {
            return 0;
        }
        out = out * 10U + (uint32_t)(*p - '0');
        p++;
        count++;
    }
    if (count == 0U) {
        return 0;
    }
    *cursor = p;
    *value = out;
    return 1;
}

static int parse_hex(const char **cursor, uint32_t *value)
{
    const char *p = *cursor;
    uint32_t out = 0U;
    uint8_t count = 0U;
    for (;;) {
        uint8_t nibble;
        if (*p >= '0' && *p <= '9') {
            nibble = (uint8_t)(*p - '0');
        } else if (*p >= 'A' && *p <= 'F') {
            nibble = (uint8_t)(*p - 'A' + 10);
        } else if (*p >= 'a' && *p <= 'f') {
            nibble = (uint8_t)(*p - 'a' + 10);
        } else {
            break;
        }
        if (count >= 8U) {
            return 0;
        }
        out = (out << 4U) | nibble;
        p++;
        count++;
    }
    if (count != 8U) {
        return 0;
    }
    *cursor = p;
    *value = out;
    return 1;
}

static int frame1200f_parse_header(psk1200f_frame_t *frame)
{
    const char *p = frame->header;
    uint32_t sequence;
    uint32_t length;
    uint32_t crc;
    if (strncmp(p, "H1200,", 6U) != 0) {
        return 0;
    }
    p += 6;
    if (!parse_decimal(&p, &sequence) || *p++ != ',' ||
        !parse_decimal(&p, &length) || *p++ != ',' ||
        !parse_hex(&p, &crc) || *p != '\0' ||
        length > PSK1200F_FRAME_PAYLOAD_CAPACITY) {
        return 0;
    }
    frame->sequence = sequence;
    frame->expected_len = (uint16_t)length;
    frame->expected_crc = crc;
    return 1;
}

static int base64_value(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

static int base64_decode(const char *text, size_t length, uint8_t *out, size_t capacity, size_t *out_len)
{
    size_t written = 0U;
    if ((length & 3U) != 0U) {
        return 0;
    }
    for (size_t pos = 0U; pos < length; pos += 4U) {
        const int v0 = base64_value(text[pos]);
        const int v1 = base64_value(text[pos + 1U]);
        const int v2 = text[pos + 2U] == '=' ? -2 : base64_value(text[pos + 2U]);
        const int v3 = text[pos + 3U] == '=' ? -2 : base64_value(text[pos + 3U]);
        if (v0 < 0 || v1 < 0 || v2 == -1 || v3 == -1 ||
            (v2 == -2 && v3 != -2) || ((v2 == -2 || v3 == -2) && pos + 4U != length)) {
            return 0;
        }
        if (written >= capacity) {
            return 0;
        }
        out[written++] = (uint8_t)((v0 << 2U) | (v1 >> 4U));
        if (v2 != -2) {
            if (written >= capacity) {
                return 0;
            }
            out[written++] = (uint8_t)((v1 << 4U) | (v2 >> 2U));
            if (v3 != -2) {
                if (written >= capacity) {
                    return 0;
                }
                out[written++] = (uint8_t)((v2 << 6U) | v3);
            }
        }
    }
    *out_len = written;
    return 1;
}

static uint32_t crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xffffffffU;
    for (size_t i = 0U; i < length; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ ((crc & 1U) ? 0xedb88320U : 0U);
        }
    }
    return ~crc;
}

static int frame1200f_should_deliver(psk1200f_frame_t *frame)
{
    if (!frame->sequence_started || frame->sequence == 0U) {
        frame->sequence_started = 1U;
        frame->next_sequence = frame->sequence + 1U;
        return 1;
    }
    if (frame->sequence < frame->next_sequence) {
        return 0;
    }
    frame->next_sequence = frame->sequence + 1U;
    return 1;
}

static void frame1200f_finish(psk_rx_t *m)
{
    psk1200f_frame_t *frame = &m->frame1200f;
    size_t payload_len = 0U;
    /* Base64 always contracts while decoding, so decoding into the input
     * buffer is safe and avoids a second multi-kilobyte Core1 allocation. */
    if (frame->delivery_pos == frame->delivery_len &&
        base64_decode(frame->base64, frame->base64_len, (uint8_t *)frame->base64,
                      sizeof(frame->base64), &payload_len) &&
        payload_len == frame->expected_len &&
        crc32((const uint8_t *)frame->base64, payload_len) == frame->expected_crc) {
        if (frame1200f_should_deliver(frame)) {
            frame->delivery_len = (uint16_t)payload_len;
            frame->delivery_pos = 0U;
        }
        frame->valid_frames++;
    } else {
        frame->invalid_frames++;
    }
    frame1200f_reset(frame);
}

static void frame1200f_deliver(psk_rx_t *m)
{
    psk1200f_frame_t *frame = &m->frame1200f;
    while (frame->delivery_pos < frame->delivery_len &&
           m->out && m->out_len + 1U < m->out_size) {
        emit_char(m, frame->base64[frame->delivery_pos++]);
    }
    if (frame->delivery_pos == frame->delivery_len) {
        frame->delivery_pos = 0U;
        frame->delivery_len = 0U;
    }
}

static void frame1200f_enter_resync(psk1200f_frame_t *frame)
{
    frame->state = PSK1200F_FRAME_RESYNC;
    frame->header_len = 0U;
    frame->base64_len = 0U;
}

static void frame1200f_feed(psk_rx_t *m, char c)
{
    psk1200f_frame_t *frame = &m->frame1200f;
    static const char prefix[] = "H1200,";
    if (frame->state == PSK1200F_FRAME_SEARCH) {
        if (c == '~') {
            frame->state = PSK1200F_FRAME_HEADER;
            frame->header_len = 0U;
        } else if (is_plaintext_char(c)) {
            emit_char(m, c);
        }
        return;
    }
    if (frame->state == PSK1200F_FRAME_RESYNC) {
        /* A bit error corrupted this frame's base64 body. The remaining
         * body bytes are not trustworthy plaintext (they are undecoded
         * base64), so discard them silently instead of falling into
         * PSK1200F_FRAME_SEARCH's literal passthrough, which would leak
         * the rest of the raw base64 payload into the RX output. Wait for
         * the next '~' to attempt a fresh frame. */
        if (c == '~') {
            frame->state = PSK1200F_FRAME_HEADER;
            frame->header_len = 0U;
        }
        return;
    }
    if (frame->state == PSK1200F_FRAME_HEADER) {
        if (c == '~') {
            frame1200f_emit_buffered_header(m, '\0');
            frame->state = PSK1200F_FRAME_HEADER;
            return;
        }
        if (frame->header_len < sizeof(prefix) - 1U &&
            c != prefix[frame->header_len]) {
            frame1200f_emit_buffered_header(m, c);
            return;
        }
        if (c == ':') {
            frame->header[frame->header_len] = '\0';
            if (frame->delivery_len == 0U && frame1200f_parse_header(frame)) {
                frame->state = PSK1200F_FRAME_DATA;
                frame->base64_len = 0U;
                frame->sync_events++;
            } else {
                frame1200f_emit_buffered_header(m, ':');
            }
            return;
        }
        if (c < 32 || c > 126 || frame->header_len + 1U >= sizeof(frame->header)) {
            frame1200f_emit_buffered_header(m, c);
            return;
        }
        frame->header[frame->header_len++] = c;
        return;
    }
    if (c == '~') {
        frame1200f_finish(m);
        return;
    }
    if ((base64_value(c) < 0 && c != '=') || frame->base64_len + 1U >= sizeof(frame->base64)) {
        frame->invalid_frames++;
        frame1200f_enter_resync(frame);
        return;
    }
    frame->base64[frame->base64_len++] = c;
}

static void on_char(char c, void *userdata)
{
    psk_rx_t *m = (psk_rx_t *)userdata;
    if (m->config.mode == PSK_MODE_8PSK1200F) {
        frame1200f_feed(m, c);
    } else {
        emit_char(m, c);
    }
}

static void backend_init(psk_rx_t *m)
{
    memset(&m->frame1200f, 0, sizeof(m->frame1200f));
    if (!psk_mode_can_decode(m->config.mode)) {
        memset(&m->rx, 0, sizeof(m->rx));
        m->backend_active = 0U;
        return;
    }

    psk_rtty_config_t cfg = {
        .mode = to_backend_mode(m->config.mode),
        .sample_rate_hz = m->config.sample_rate_hz,
        .rx_tone_hz = m->config.rx_tone_hz,
        .char_cb = on_char,
        .userdata = m,
    };
    psk_rtty_rx_init(&m->rx, &cfg);
    m->rx.sq_thresh = m->config.squelch;
    m->rx.afc_enabled = m->config.afc_enabled ? 1 : 0;
    m->rx.squelch_enabled = m->config.squelch_enabled ? 1 : 0;
    m->backend_active = 1U;
    if (m->fec_branch_override == 0 || m->fec_branch_override == 1) {
        psk_rtty_rx_set_fec_branch_override(&m->rx, m->fec_branch_override);
    }
}

static int backend_effective_tone_hz(const psk_rx_t *m)
{
    if (!m->backend_active) {
        return m->config.rx_tone_hz;
    }
    float phase_inc = m->rx.nco_phase_inc + m->rx.freq_error;
    if (!m->rx.afc_enabled || m->rx.carriers != 1U) {
        phase_inc = m->rx.carrier_phase_inc[0];
    }
    float hz = phase_inc * (float)m->rx.config.sample_rate_hz / MODEM_TWO_PI;
    if (hz < 0.0f) {
        hz = 0.0f;
    }
    return (int)(hz + 0.5f);
}

void psk_rx_init(psk_rx_t *m, const psk_rx_config_t *config)
{
    memset(m, 0, sizeof(*m));
    m->config = *config;
    m->fec_branch_override = -1;
    backend_init(m);
}

void psk_rx_reset(psk_rx_t *m)
{
    backend_init(m);
}

void psk_rx_set_sample_rate(psk_rx_t *m, int sample_rate_hz)
{
    if (!m || sample_rate_hz <= 0) {
        return;
    }
    if (m->config.sample_rate_hz == sample_rate_hz) {
        return;
    }
    m->config.sample_rate_hz = sample_rate_hz;
    backend_init(m);
}

void psk_rx_set_mode(psk_rx_t *m, psk_mode_t mode)
{
    m->config.mode = mode;
    backend_init(m);
}

void psk_rx_set_tone(psk_rx_t *m, int tone_hz)
{
    if (tone_hz < 100) {
        tone_hz = 100;
    } else if (tone_hz > 3500) {
        tone_hz = 3500;
    }
    m->config.rx_tone_hz = tone_hz;
    backend_init(m);
}

void psk_rx_set_afc(psk_rx_t *m, int enabled)
{
    m->config.afc_enabled = enabled ? 1 : 0;
    if (m->backend_active) {
        m->rx.afc_enabled = m->config.afc_enabled;
        if (!m->rx.afc_enabled) {
            m->rx.freq_error = 0.0f;
            m->rx.q_freq_error = 0.0f;
            m->rx.fferr_ave = 0.0f;
            m->rx.fperr_ave = 0.0f;
            m->rx.nco_phase_inc = MODEM_TWO_PI *
                                  (float)m->rx.config.rx_tone_hz /
                                  (float)m->rx.config.sample_rate_hz;
        }
    }
}

void psk_rx_set_squelch_enabled(psk_rx_t *m, int enabled)
{
    m->config.squelch_enabled = enabled ? 1 : 0;
    if (m->backend_active) {
        m->rx.squelch_enabled = m->config.squelch_enabled;
    }
}

void psk_rx_set_fec_branch_override(psk_rx_t *m, int branch)
{
    m->fec_branch_override = (branch == 0 || branch == 1) ? branch : -1;
    if (m->backend_active) {
        psk_rtty_rx_set_fec_branch_override(&m->rx, m->fec_branch_override);
    }
}

psk_mode_t psk_rx_mode(const psk_rx_t *m)
{
    return m->config.mode;
}

int psk_rx_tone_hz(const psk_rx_t *m)
{
    return backend_effective_tone_hz(m);
}

const char *psk_rx_mode_name(psk_mode_t mode)
{
    return psk_mode_name(mode);
}

const psk_mode_info_t *psk_rx_mode_info(const psk_rx_t *m)
{
    return psk_mode_info(m->config.mode);
}

int psk_rx_mode_supported(psk_mode_t mode)
{
    return psk_mode_can_decode(mode);
}

void psk_rx_debug_snapshot(const psk_rx_t *m, psk_rx_debug_t *debug)
{
    if (!debug) {
        return;
    }

    debug->symbols = m->rx.debug_symbols;
    debug->bits = m->rx.debug_bits;
    debug->ones = m->rx.debug_ones;
    debug->zeros = m->rx.debug_zeros;
    debug->delims = m->rx.debug_delims;
    debug->chars = m->rx.debug_chars;
    debug->unknown = m->rx.debug_unknown;
    debug->sq_open = m->rx.sq_open;
    debug->sq_level = m->rx.sq_level;
    debug->sq_thresh = m->rx.sq_thresh;
    debug->afc_enabled = m->rx.afc_enabled;
    debug->squelch_enabled = m->rx.squelch_enabled;
    debug->stable_symbols = m->rx.sq_stable_symbols;
    debug->warmup_symbols = m->rx.warmup_symbols;
    debug->tone_hz = backend_effective_tone_hz(m);
    debug->selected_tone_hz = m->backend_active ? m->rx.config.rx_tone_hz : m->config.rx_tone_hz;
    debug->fec_metric[0] = m->rx.debug_fec_metric[0];
    debug->fec_metric[1] = m->rx.debug_fec_metric[1];
    debug->fec_selected = m->rx.debug_fec_selected;
    debug->lock_state = m->rx.lock_state;
    debug->timing_bins = m->rx.timing_bins;
    debug->timing_peak = m->rx.raw_pk_pos;
    debug->timing_target = m->rx.pk_pos;
    debug->carrier_offset_millihz = (int)(m->rx.freq_error *
        (float)m->rx.config.sample_rate_hz * 1000.0f / MODEM_TWO_PI);
    debug->timing_adjusts = m->rx.debug_timing_adjusts;
    debug->soft_ambiguous = m->rx.debug_soft_ambiguous;
    debug->coarse_updates = m->rx.debug_coarse_updates;
    debug->valid_frames = m->frame1200f.valid_frames;
    debug->invalid_frames = m->frame1200f.invalid_frames;
    debug->frame_sync_events = m->frame1200f.sync_events;
    debug->fec_pairs[0] = m->rx.debug_fec_pairs[0];
    debug->fec_pairs[1] = m->rx.debug_fec_pairs[1];
    debug->fec_pair_count = m->rx.debug_fec_pair_count;
    debug->fec_vote_candidate = m->rx.debug_fec_vote_candidate;
    debug->fec_vote_count = m->rx.debug_fec_vote_count;
    debug->fec_output_enabled = m->rx.debug_fec_output_enabled;
    debug->fec_locked_branch = m->rx.debug_fec_locked_branch;
}

size_t psk_rx_process(psk_rx_t *m, const int16_t *samples, size_t count,
                         char *out, size_t out_size)
{
    if (out_size == 0) {
        return 0;
    }
    out[0] = '\0';
    if (!m->backend_active) {
        return 0;
    }
    m->out = out;
    m->out_size = out_size;
    m->out_len = 0;
    psk_rtty_rx_feed(&m->rx, samples, count);
    if (m->config.mode == PSK_MODE_8PSK1200F) {
        frame1200f_deliver(m);
    }
    m->out = NULL;
    return m->out_len;
}
