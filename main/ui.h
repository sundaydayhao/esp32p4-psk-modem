#pragma once

#include "display.h"
#include "esp_err.h"
#include "rx_modem.h"

typedef enum {
    UI_EVENT_NONE,
    UI_EVENT_MODE_CHANGED,
    UI_EVENT_PSK_MODE_CHANGED,
    UI_EVENT_TRANSPORT_CHANGED,
    UI_EVENT_TONE_CHANGED,
    UI_EVENT_TX_MODE_CHANGED,
    UI_EVENT_DISPLAY_ROLE_CHANGED,
    UI_EVENT_TX_TEXT,
    UI_EVENT_OUTPUT_GAIN,
    UI_EVENT_INPUT_GAIN,
    UI_EVENT_RX_DIAG,
    UI_EVENT_RX_FEC_BRANCH,
    UI_EVENT_RX_CONFIG,
    UI_EVENT_TX_STOP
} ui_event_type_t;

typedef struct {
    ui_event_type_t type;
    modem_mode_t mode;
    psk_mode_t psk_mode;
    psk_mode_t tx_psk_mode;
    transport_mode_t transport_mode;
    int tone_hz;
    int tx_tone_hz;
    int display_role;
    int output_dac_volume;
    int output_hp_volume;
    float input_gain_db;
    int fec_branch_override;
    char tx_text[256];
} ui_event_t;

esp_err_t ui_init(void);
ui_event_t ui_poll(const display_state_t *display_state);
