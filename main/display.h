#pragma once

#include "esp_err.h"
#include "modem_types.h"
#include "psk_modes.h"
#include "spectrum.h"
#include "transport.h"

typedef enum {
    DISPLAY_ROLE_RX = 0,
    DISPLAY_ROLE_TX = 1,
} display_role_t;

typedef void (*display_mode_selected_cb_t)(psk_mode_t mode, void *user);
typedef void (*display_role_selected_cb_t)(display_role_t role, void *user);
typedef void (*display_tx_text_selected_cb_t)(uint8_t index, void *user);

typedef struct {
    modem_mode_t selected_mode;
    psk_mode_t selected_psk_mode;
    transport_mode_t transport_mode;
    display_role_t display_role;
    int rx_tone_hz;
    uint32_t tx_active;
    uint32_t tx_queued;
    uint16_t input_abs_avg;
    uint8_t selected_lane;
    char text[1024];
    size_t text_len;
} display_state_t;

esp_err_t display_init(void);
void display_append_text(display_state_t *state, const char *text);
void display_render(const display_state_t *state, const spectrum_t *spectrum);
int display_take_tone_selection(int *tone_hz);
void display_task(void);
void display_task_handler(void);
void display_status(const char *line1, const char *line2);
void display_set_mode_selected_callback(display_mode_selected_cb_t cb, void *user);
void display_set_role_selected_callback(display_role_selected_cb_t cb, void *user);
void display_set_tx_text_selected_callback(display_tx_text_selected_cb_t cb, void *user);
void display_set_tx_text_selection(uint8_t index);
