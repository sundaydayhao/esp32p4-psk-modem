#include "uart_tx_pump.h"

#include <string.h>

int uart_tx_pump_begin(uart_tx_pump_t *pump, const uint8_t *data, size_t len,
                       uint32_t now_ms)
{
    if (!pump || !data || len == 0U || len > sizeof(pump->data) || pump->length) {
        return -1;
    }
    memcpy(pump->data, data, len);
    pump->length = len;
    pump->offset = 0U;
    pump->started_ms = now_ms;
    return 0;
}

uart_tx_pump_result_t uart_tx_pump_step(uart_tx_pump_t *pump, uint32_t now_ms,
                                       uart_tx_write_fn write, uart_tx_idle_fn idle,
                                       void *user)
{
    if (!pump->length) {
        return UART_TX_PUMP_IDLE;
    }
    /* Completion means the shift register drained, not just FIFO acceptance. */
    if (pump->offset == pump->length && idle(user)) {
        pump->length = 0U;
        ++pump->completed_frames;
        return UART_TX_PUMP_COMPLETE;
    }
    if ((uint32_t)(now_ms - pump->started_ms) >= UART_TX_PUMP_TIMEOUT_MS) {
        pump->length = 0U;
        ++pump->timeouts;
        return UART_TX_PUMP_TIMEOUT;
    }
    if (pump->offset < pump->length) {
        const size_t remaining = pump->length - pump->offset;
        const size_t written = write(user, pump->data + pump->offset, remaining);
        if (written <= remaining) {
            pump->offset += written;
            pump->accepted_bytes += (uint32_t)written;
        }
    }
    return UART_TX_PUMP_PENDING;
}

void uart_tx_pump_cancel(uart_tx_pump_t *pump)
{
    if (pump->length) {
        ++pump->cancelled_frames;
    }
    pump->length = 0U;
    pump->offset = 0U;
}
