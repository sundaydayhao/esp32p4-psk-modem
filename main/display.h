#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "rx_modem.h"
#include "spectrum.h"
#include "transport.h"
#include "content_service.h"

typedef struct {
    char text[1024];
    char tx_text[256];
    size_t text_len;
    modem_mode_t selected_mode;
    psk_mode_t selected_psk_mode;
    psk_mode_t selected_tx_psk_mode;
    transport_mode_t transport_mode;
    int rx_tone_hz;
    int tx_tone_hz;
    int display_role;
    int tx_active;
    int update_waterfall;
    size_t tx_queued;
    uint16_t input_abs_avg;
    uint8_t selected_lane;
    content_type_t content_type;
    uint32_t content_epoch;
    uint32_t codec_rate;
} display_state_t;

esp_err_t display_init(void);
void display_append_text(display_state_t *state, const char *text);
void display_render(const display_state_t *state, const spectrum_t *spectrum);
int display_take_tone_selection(int *tone_hz);
int display_take_mode_selection(psk_mode_t *mode);
int display_take_role_selection(int *display_role);
int display_take_content_selection(content_type_t *content);
void display_task_handler(void);
