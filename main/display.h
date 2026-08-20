#pragma once

#include "esp_err.h"
#include "rx_modem.h"
#include "spectrum.h"
#include "transport.h"

typedef struct {
    char text[1024];
    size_t text_len;
    modem_mode_t selected_mode;
    psk_mode_t selected_psk_mode;
    transport_mode_t transport_mode;
    int rx_tone_hz;
} display_state_t;

esp_err_t display_init(void);
void display_append_text(display_state_t *state, const char *text);
void display_render(const display_state_t *state, const spectrum_t *spectrum);
int display_take_tone_selection(int *tone_hz);
void display_task_handler(void);
