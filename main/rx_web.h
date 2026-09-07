#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "psk_modes.h"

typedef struct {
    psk_mode_t mode;
    int tone_hz;
    unsigned lock_state;
    unsigned input_level;
    unsigned samples_per_second;
    unsigned dsp_load_permille;
    unsigned dsp_max_us;
    unsigned block_budget_us;
    uint32_t deadline_misses;
    uint32_t read_errors;
} rx_web_status_t;

typedef bool (*rx_web_config_cb_t)(psk_mode_t mode, int tone_hz);

/* Starts on core 1; a missing C6 does not prevent the audio tasks starting. */
esp_err_t rx_web_start(rx_web_config_cb_t config_cb);
void rx_web_set_status(const rx_web_status_t *status);
void rx_web_get_status(rx_web_status_t *status);
