#include "rx_modem.h"

#include <string.h>

static const char *const s_mode_names[MODEM_MODE_COUNT] = {
    "BPSK31",
    "PSK63",
    "FT8",
    "FT4",
    "RTTY",
    "CW",
    "MFSK16",
};

static void on_decoder_bytes(const uint8_t *data, size_t len, void *userdata)
{
    modem_t *m = (modem_t *)userdata;
    if (!m->out || !data || len == 0 || m->out_len >= m->out_size) {
        return;
    }
    const size_t room = m->out_size - m->out_len - 1;
    if (len > room) {
        len = room;
    }
    memcpy(m->out + m->out_len, data, len);
    m->out_len += len;
    m->out[m->out_len] = '\0';
}

static void recreate_decoder(modem_t *m)
{
    decoder_layer_config_t config = {
        .mode = m->config.mode,
        .psk_mode = m->config.psk_mode,
        .sample_rate_hz = m->config.sample_rate_hz,
        .rx_tone_hz = m->config.rx_tone_hz,
        .squelch = m->config.squelch,
        .emit_cb = on_decoder_bytes,
        .userdata = m,
    };
    decoder_layer_init(&m->decoder, &config);
}

void modem_init(modem_t *m, const modem_config_t *config)
{
    memset(m, 0, sizeof(*m));
    m->config = *config;
    if (m->config.mode == MODEM_MODE_BPSK31) {
        m->config.psk_mode = PSK_MODE_BPSK31;
    } else if (m->config.mode == MODEM_MODE_PSK63 && m->config.psk_mode == PSK_MODE_BPSK31) {
        m->config.psk_mode = PSK_MODE_BPSK63;
    }
    recreate_decoder(m);
}

void modem_set_mode(modem_t *m, modem_mode_t mode)
{
    if (mode >= MODEM_MODE_COUNT) {
        return;
    }
    m->config.mode = mode;
    if (mode == MODEM_MODE_BPSK31) {
        m->config.psk_mode = PSK_MODE_BPSK31;
    } else if (mode == MODEM_MODE_PSK63 && m->config.psk_mode == PSK_MODE_BPSK31) {
        m->config.psk_mode = PSK_MODE_BPSK63;
    }
    decoder_layer_set_mode(&m->decoder, mode);
}

void modem_set_psk_mode(modem_t *m, psk_mode_t mode)
{
    if ((unsigned)mode >= PSK_MODE_COUNT) {
        return;
    }
    m->config.psk_mode = mode;
    m->config.mode = mode == PSK_MODE_BPSK31 ? MODEM_MODE_BPSK31 : MODEM_MODE_PSK63;
    decoder_layer_set_psk_mode(&m->decoder, mode);
}

void modem_set_rx_tone(modem_t *m, int tone_hz)
{
    if (tone_hz < 100) {
        tone_hz = 100;
    } else if (tone_hz > 3500) {
        tone_hz = 3500;
    }
    m->config.rx_tone_hz = tone_hz;
    decoder_layer_set_tone(&m->decoder, tone_hz);
}

modem_mode_t modem_get_mode(const modem_t *m)
{
    return m->config.mode;
}

psk_mode_t modem_get_psk_mode(const modem_t *m)
{
    return m->config.psk_mode;
}

const char *modem_mode_name(modem_mode_t mode)
{
    return mode < MODEM_MODE_COUNT ? s_mode_names[mode] : "?";
}

const char *modem_psk_mode_name(psk_mode_t mode)
{
    return psk_mode_name(mode);
}

int modem_mode_band_min_hz(modem_mode_t mode)
{
    const int center = 1500;
    const int half = demod_profile_for_mode(mode)->bandwidth_hz / 2;
    return center - half;
}

int modem_mode_band_max_hz(modem_mode_t mode)
{
    const int center = 1500;
    const int half = demod_profile_for_mode(mode)->bandwidth_hz / 2;
    return center + half;
}

int modem_mode_decoder_available(modem_mode_t mode)
{
    return mode < MODEM_MODE_COUNT;
}

int modem_get_rx_tone(const modem_t *m)
{
    return m->config.rx_tone_hz;
}

size_t modem_process(modem_t *m, const int16_t *samples, size_t count, char *out, size_t out_size)
{
    if (out_size == 0) {
        return 0;
    }
    out[0] = '\0';
    m->out = out;
    m->out_size = out_size;
    m->out_len = 0;

    decoder_layer_feed(&m->decoder, samples, count);
    if (m->out_len > 0) {
        return m->out_len;
    }

    m->sample_counter += count;

    out[0] = '\0';
    return 0;
}
