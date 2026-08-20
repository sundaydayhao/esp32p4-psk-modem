#pragma once

#include <stddef.h>
#include <stdint.h>

#include "modem_types.h"
#include "rx_decoder.h"

typedef struct {
    modem_mode_t mode;
    psk_mode_t psk_mode;
    int sample_rate_hz;
    int rx_tone_hz;
    int squelch;
} modem_config_t;

typedef struct modem modem_t;

void modem_init(modem_t *m, const modem_config_t *config);
void modem_set_mode(modem_t *m, modem_mode_t mode);
void modem_set_psk_mode(modem_t *m, psk_mode_t mode);
void modem_set_rx_tone(modem_t *m, int tone_hz);
modem_mode_t modem_get_mode(const modem_t *m);
psk_mode_t modem_get_psk_mode(const modem_t *m);
const char *modem_mode_name(modem_mode_t mode);
const char *modem_psk_mode_name(psk_mode_t mode);
int modem_mode_band_min_hz(modem_mode_t mode);
int modem_mode_band_max_hz(modem_mode_t mode);
int modem_mode_decoder_available(modem_mode_t mode);
int modem_get_rx_tone(const modem_t *m);
size_t modem_process(modem_t *m, const int16_t *samples, size_t count, char *out, size_t out_size);

struct modem {
    modem_config_t config;
    uint32_t sample_counter;
    decoder_layer_t decoder;
    char *out;
    size_t out_size;
    size_t out_len;
};
