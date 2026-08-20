#pragma once

#include "display.h"
#include "esp_err.h"
#include "rx_modem.h"
#include "psk_tx.h"

typedef enum {
    UI_EVENT_NONE,
    UI_EVENT_MODE_CHANGED,
    UI_EVENT_PSK_MODE_CHANGED,
    UI_EVENT_TRANSPORT_CHANGED,
    UI_EVENT_TONE_CHANGED,
    UI_EVENT_TX_REQUEST
} ui_event_type_t;

typedef struct {
    ui_event_type_t type;
    modem_mode_t mode;
    psk_mode_t psk_mode;
    transport_mode_t transport_mode;
    int tone_hz;
    tx_request_t tx_request;
} ui_event_t;

esp_err_t ui_init(void);
ui_event_t ui_poll(const display_state_t *display_state);
