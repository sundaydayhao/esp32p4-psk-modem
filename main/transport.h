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
size_t transport_read(uint64_t *cursor, uint8_t *out, size_t capacity, uint64_t *lost, uint32_t *epoch);
void transport_begin_session(uint32_t epoch);
uint64_t transport_received_bytes(void);
uint64_t transport_usb_dropped_bytes(void);
