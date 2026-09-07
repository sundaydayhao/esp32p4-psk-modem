#pragma once

#include <stddef.h>
#include <stdint.h>

#define UART_TX_PUMP_CAPACITY 16U
#define UART_TX_PUMP_TIMEOUT_MS 40U

typedef size_t (*uart_tx_write_fn)(void *user, const uint8_t *data, size_t len);
typedef int (*uart_tx_idle_fn)(void *user);

typedef enum {
    UART_TX_PUMP_IDLE,
    UART_TX_PUMP_PENDING,
    UART_TX_PUMP_COMPLETE,
    UART_TX_PUMP_TIMEOUT
} uart_tx_pump_result_t;

typedef struct {
    uint8_t data[UART_TX_PUMP_CAPACITY];
    size_t length;
    size_t offset;
    uint32_t started_ms;
    volatile uint32_t accepted_bytes;
    volatile uint32_t completed_frames;
    volatile uint32_t timeouts;
    volatile uint32_t cancelled_frames;
} uart_tx_pump_t;

/* Single UART task owner. Callbacks must not wait for hardware. */
int uart_tx_pump_begin(uart_tx_pump_t *pump, const uint8_t *data, size_t len,
                       uint32_t now_ms);
uart_tx_pump_result_t uart_tx_pump_step(uart_tx_pump_t *pump, uint32_t now_ms,
                                       uart_tx_write_fn write, uart_tx_idle_fn idle,
                                       void *user);
void uart_tx_pump_cancel(uart_tx_pump_t *pump);
