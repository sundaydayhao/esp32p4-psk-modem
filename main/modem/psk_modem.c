#include "psk_modem.h"

#include <string.h>

#define MODEM_TWO_PI 6.28318530717958647692f
#define MODEM_PSK_FAMILY_8PSK 2U

static psk_rtty_mode_t to_backend_mode(psk_mode_t mode)
{
    const psk_mode_info_t *info = psk_mode_info(mode);
    return info->backend_mode;
}

static void on_char(char c, void *userdata)
{
    psk_modem_t *m = (psk_modem_t *)userdata;
    if (!m->out || m->out_len + 1 >= m->out_size) {
        return;
    }
    m->out[m->out_len++] = c;
    m->out[m->out_len] = '\0';
}

static void backend_init(psk_modem_t *m)
{
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
}

static int backend_effective_tone_hz(const psk_modem_t *m)
{
    if (!m->backend_active) {
        return m->config.rx_tone_hz;
    }
    float phase_inc = m->rx.nco_phase_inc + m->rx.freq_error;
    if (!m->rx.afc_enabled || m->rx.carriers != 1U || m->rx.psk_family == MODEM_PSK_FAMILY_8PSK) {
        phase_inc = m->rx.carrier_phase_inc[0];
    }
    float hz = phase_inc * (float)m->rx.config.sample_rate_hz / MODEM_TWO_PI;
    if (hz < 0.0f) {
        hz = 0.0f;
    }
    return (int)(hz + 0.5f);
}

void psk_modem_init(psk_modem_t *m, const psk_modem_config_t *config)
{
    memset(m, 0, sizeof(*m));
    m->config = *config;
    backend_init(m);
}

void psk_modem_set_mode(psk_modem_t *m, psk_mode_t mode)
{
    m->config.mode = mode;
    backend_init(m);
}

void psk_modem_set_tone(psk_modem_t *m, int tone_hz)
{
    if (tone_hz < 100) {
        tone_hz = 100;
    } else if (tone_hz > 3500) {
        tone_hz = 3500;
    }
    m->config.rx_tone_hz = tone_hz;
    backend_init(m);
}

void psk_modem_set_afc(psk_modem_t *m, int enabled)
{
    m->config.afc_enabled = enabled ? 1 : 0;
    if (m->backend_active) {
        m->rx.afc_enabled = m->config.afc_enabled;
        if (!m->rx.afc_enabled) {
            m->rx.freq_error = 0.0f;
            m->rx.fferr_ave = 0.0f;
            m->rx.fperr_ave = 0.0f;
            m->rx.nco_phase_inc = MODEM_TWO_PI *
                                  (float)m->rx.config.rx_tone_hz /
                                  (float)m->rx.config.sample_rate_hz;
        }
    }
}

void psk_modem_set_squelch_enabled(psk_modem_t *m, int enabled)
{
    m->config.squelch_enabled = enabled ? 1 : 0;
    if (m->backend_active) {
        m->rx.squelch_enabled = m->config.squelch_enabled;
    }
}

psk_mode_t psk_modem_mode(const psk_modem_t *m)
{
    return m->config.mode;
}

int psk_modem_tone_hz(const psk_modem_t *m)
{
    return backend_effective_tone_hz(m);
}

const char *psk_modem_mode_name(psk_mode_t mode)
{
    return psk_mode_name(mode);
}

const psk_mode_info_t *psk_modem_mode_info(const psk_modem_t *m)
{
    return psk_mode_info(m->config.mode);
}

int psk_modem_mode_supported(psk_mode_t mode)
{
    return psk_mode_can_decode(mode);
}

void psk_modem_debug_snapshot(const psk_modem_t *m, psk_modem_debug_t *debug)
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
}

size_t psk_modem_process(psk_modem_t *m, const int16_t *samples, size_t count,
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
    m->out = NULL;
    return m->out_len;
}
