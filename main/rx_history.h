#pragma once

#include <stddef.h>
#include <stdint.h>

#define RX_HISTORY_CAPACITY 16384U

/* Caller serializes access. Independent cursors never consume another reader's data. */
typedef struct {
    uint8_t bytes[RX_HISTORY_CAPACITY];
    uint64_t total;
    uint64_t session_start;
} rx_history_t;

void rx_history_append(rx_history_t *history, const uint8_t *data, size_t len);
size_t rx_history_read(const rx_history_t *history, uint64_t *cursor,
                       uint8_t *out, size_t capacity, uint64_t *lost);
