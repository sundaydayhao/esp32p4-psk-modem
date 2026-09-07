#include "uart_tx_pump.h"
#include "ap580_playout.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t bytes[1024];
    size_t count;
    size_t allowance;
    unsigned write_calls;
    unsigned idle_calls;
    int idle;
} fake_uart_t;

static size_t fake_write(void *user, const uint8_t *bytes, size_t len)
{
    fake_uart_t *uart = user;
    ++uart->write_calls;
    size_t count = len < uart->allowance ? len : uart->allowance;
    assert(uart->count + count <= sizeof(uart->bytes));
    memcpy(uart->bytes + uart->count, bytes, count);
    uart->count += count;
    return count;
}

static int fake_idle(void *user)
{
    fake_uart_t *uart = user;
    ++uart->idle_calls;
    return uart->idle;
}

static uart_tx_pump_result_t step(uart_tx_pump_t *pump, uint32_t now, fake_uart_t *uart)
{
    unsigned calls = uart->write_calls;
    uart_tx_pump_result_t result = uart_tx_pump_step(pump, now, fake_write, fake_idle, uart);
    assert(uart->write_calls - calls <= 1U);
    return result;
}

static void test_partial_and_stall(void)
{
    uart_tx_pump_t pump = {0};
    fake_uart_t uart = {0};
    const uint8_t frame[16] = {0x59, 0x4f, 1, 7, 0, 1, 2, 3, 4, 5, 6, 0, 0, 0, 0, 99};
    assert(step(&pump, 0, &uart) == UART_TX_PUMP_IDLE);
    assert(uart_tx_pump_begin(&pump, frame, 17, 0) == -1);
    assert(uart_tx_pump_begin(&pump, frame, 0, 0) == -1);
    assert(uart_tx_pump_begin(&pump, frame, 16, 100) == 0);
    assert(uart_tx_pump_begin(&pump, frame, 16, 101) == -1);
    for (uint32_t tick = 100; tick < 105; ++tick) {
        assert(step(&pump, tick, &uart) == UART_TX_PUMP_PENDING);
    }
    assert(pump.offset == 0);
    uart.allowance = 3;
    for (uint32_t tick = 105; tick < 111; ++tick) {
        assert(step(&pump, tick, &uart) == UART_TX_PUMP_PENDING);
    }
    assert(pump.offset == 16 && pump.accepted_bytes == 16);
    assert(memcmp(frame, uart.bytes, 16) == 0);
    assert(step(&pump, 111, &uart) == UART_TX_PUMP_PENDING);
    uart.idle = 1;
    assert(step(&pump, 112, &uart) == UART_TX_PUMP_COMPLETE);
    assert(pump.completed_frames == 1 && pump.timeouts == 0);
    assert(step(&pump, 113, &uart) == UART_TX_PUMP_IDLE);
}

static void test_timeout_and_cancel(void)
{
    uart_tx_pump_t pump = {0};
    fake_uart_t uart = {0};
    uint8_t frame[16] = {1};
    const uint32_t start = UINT32_MAX - 20U;
    assert(uart_tx_pump_begin(&pump, frame, 16, start) == 0);
    for (uint32_t tick = 0; tick < UART_TX_PUMP_TIMEOUT_MS; ++tick) {
        assert(step(&pump, start + tick, &uart) == UART_TX_PUMP_PENDING);
    }
    assert(step(&pump, start + UART_TX_PUMP_TIMEOUT_MS, &uart) == UART_TX_PUMP_TIMEOUT);
    assert(pump.timeouts == 1 && uart.count == 0);
    assert(step(&pump, 30, &uart) == UART_TX_PUMP_IDLE);
    uart.allowance = 4;
    assert(uart_tx_pump_begin(&pump, frame, 16, 40) == 0);
    assert(step(&pump, 40, &uart) == UART_TX_PUMP_PENDING);
    assert(pump.offset == 4);
    uart_tx_pump_cancel(&pump);
    assert(pump.cancelled_frames == 1);
    uart_tx_pump_cancel(&pump);
    assert(pump.cancelled_frames == 1);
    memset(frame, 2, sizeof(frame));
    uart.allowance = 16;
    assert(uart_tx_pump_begin(&pump, frame, 16, 41) == 0);
    assert(step(&pump, 41, &uart) == UART_TX_PUMP_PENDING);
    assert(memcmp(uart.bytes + 4, frame, 16) == 0);
    /* Even a stuck shift register cannot hold the task forever. */
    assert(step(&pump, 81, &uart) == UART_TX_PUMP_TIMEOUT);
    assert(pump.timeouts == 2 && pump.completed_frames == 0);
    uart.idle = 1;
    assert(uart_tx_pump_begin(&pump, frame, 1, 82) == 0);
    assert(step(&pump, 82, &uart) == UART_TX_PUMP_PENDING);
    assert(step(&pump, 83, &uart) == UART_TX_PUMP_COMPLETE);
}

static void test_playout(uint32_t rate)
{
    uart_tx_pump_t pump = {0};
    fake_uart_t uart = {.allowance = 16};
    ap580_playout_t playout;
    ap580_playout_reset(&playout, rate, 0);
    uint8_t frame[16] = {0x59, 0x4f, 1, 7};
    unsigned fresh = 0;
    uint32_t drained_at = 0;
    for (uint32_t now = 0; now < 2000; ++now) {
        uart.idle = now >= drained_at;
        size_t before = uart.count;
        assert(step(&pump, now, &uart) != UART_TX_PUMP_TIMEOUT);
        if (uart.count > before) {
            drained_at = now + 17; /* Worst configured UART speed: 9600. */
        }
        if (!pump.length && ap580_playout_due(&playout, now)) {
            const uint8_t *input = fresh < 4 ? frame : NULL;
            if (input) ++fresh;
            if (ap580_playout_next(&playout, input, now, frame) == AP580_PLAYOUT_VOICE) {
                assert(uart_tx_pump_begin(&pump, frame, 16, now) == 0);
            }
        }
    }
    assert(fresh == 4);
    assert(pump.completed_frames == 6); /* Four fresh + at most two repeats. */
    assert(pump.accepted_bytes == 96 && pump.timeouts == 0);
}

int main(void)
{
    test_partial_and_stall();
    test_timeout_and_cancel();
    test_playout(600);
    test_playout(1200);
    test_playout(2400);
    puts("UART TX pump: partial/stall/timeout/cancel/wrap + 3-rate playout PASS");
    return 0;
}
