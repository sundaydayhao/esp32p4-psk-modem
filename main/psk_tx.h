#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "psk_modes.h"
#include "modem_types.h"

typedef struct {
    modem_mode_t mode;
    psk_mode_t psk_mode;
    int tone_hz;
    char text[128];
} tx_request_t;

esp_err_t tx_init(void);
esp_err_t tx_submit(const tx_request_t *request);
void tx_task(void *arg);
