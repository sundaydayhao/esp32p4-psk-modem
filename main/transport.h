#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    TRANSPORT_MODE_USB_SERIAL = 0,
    TRANSPORT_MODE_WIFI,
    TRANSPORT_MODE_BT,
    TRANSPORT_MODE_COUNT
} transport_mode_t;

esp_err_t transport_init(transport_mode_t mode);
esp_err_t transport_set_mode(transport_mode_t mode);
transport_mode_t transport_get_mode(void);
const char *transport_mode_name(transport_mode_t mode);
void transport_send(const uint8_t *data, size_t len);
