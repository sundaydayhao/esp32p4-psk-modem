#pragma once

#include "display.h"
#include "esp_err.h"
#include "modem.h"

typedef enum {
    UI_EVENT_NONE,
    UI_EVENT_MODE_CHANGED,
    UI_EVENT_PSK_MODE_CHANGED,
    UI_EVENT_TRANSPORT_CHANGED,
    UI_EVENT_TONE_CHANGED
} ui_event_type_t;

typedef struct {
    ui_event_type_t type;
    modem_mode_t mode;
    psk_mode_t psk_mode;
    transport_mode_t transport_mode;
    int tone_hz;
} ui_event_t;

esp_err_t ui_init(void);
ui_event_t ui_poll(const display_state_t *display_state);
