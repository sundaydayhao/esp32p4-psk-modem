#pragma once

#include <stddef.h>
#include <stdint.h>

#include "psk_modes.h"
#include "psk_rtty.h"

typedef struct {
    psk_mode_t mode;
    int sample_rate_hz;
    int rx_tone_hz;
    int squelch;
    int afc_enabled;
    int squelch_enabled;
} psk_modem_config_t;

typedef struct {
    psk_modem_config_t config;
    psk_rtty_rx_t rx;
    char *out;
    size_t out_size;
    size_t out_len;
    uint8_t backend_active;
} psk_modem_t;

typedef struct {
    uint32_t symbols;
    uint32_t bits;
    uint32_t ones;
    uint32_t zeros;
    uint32_t delims;
    uint32_t chars;
    uint32_t unknown;
    int sq_open;
    int sq_level;
    int sq_thresh;
    int afc_enabled;
    int squelch_enabled;
    int stable_symbols;
    int warmup_symbols;
    int tone_hz;
    int selected_tone_hz;
    int fec_metric[2];
    uint8_t fec_selected;
} psk_modem_debug_t;

void psk_modem_init(psk_modem_t *m, const psk_modem_config_t *config);
void psk_modem_set_mode(psk_modem_t *m, psk_mode_t mode);
void psk_modem_set_tone(psk_modem_t *m, int tone_hz);
void psk_modem_set_afc(psk_modem_t *m, int enabled);
void psk_modem_set_squelch_enabled(psk_modem_t *m, int enabled);
psk_mode_t psk_modem_mode(const psk_modem_t *m);
int psk_modem_tone_hz(const psk_modem_t *m);
const char *psk_modem_mode_name(psk_mode_t mode);
const psk_mode_info_t *psk_modem_mode_info(const psk_modem_t *m);
int psk_modem_mode_supported(psk_mode_t mode);
void psk_modem_debug_snapshot(const psk_modem_t *m, psk_modem_debug_t *debug);
size_t psk_modem_process(psk_modem_t *m, const int16_t *samples, size_t count,
                         char *out, size_t out_size);
