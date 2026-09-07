#include "rx_history.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    static rx_history_t h;
    static uint8_t source[RX_HISTORY_CAPACITY * 2 + 37], out[RX_HISTORY_CAPACITY];
    for (size_t i = 0; i < sizeof(source); ++i) source[i] = (uint8_t)(i * 73 + i / 251);
    uint64_t a = 0, b = 0, lost;
    rx_history_append(&h, source, 700);
    assert(rx_history_read(&h, &a, out, 300, &lost) == 300 && lost == 0);
    assert(memcmp(out, source, 300) == 0);
    assert(rx_history_read(&h, &b, out, sizeof(out), &lost) == 700 && lost == 0);
    assert(memcmp(out, source, 700) == 0);
    rx_history_append(&h, source + 700, sizeof(source) - 700);
    assert(rx_history_read(&h, &a, out, sizeof(out), &lost) == RX_HISTORY_CAPACITY);
    assert(lost == sizeof(source) - RX_HISTORY_CAPACITY - 300);
    assert(memcmp(out, source + sizeof(source) - RX_HISTORY_CAPACITY, sizeof(out)) == 0);
    assert(rx_history_read(&h, &a, out, 10, &lost) == 0 && lost == 0);
    b = h.total + 123; /* stale cursor after board reboot */
    assert(rx_history_read(&h, &b, out, sizeof(out), &lost) == RX_HISTORY_CAPACITY);
    assert(memcmp(out, source + sizeof(source) - RX_HISTORY_CAPACITY, sizeof(out)) == 0);
    rx_history_append(&h, source, 123);
    assert(rx_history_read(&h, &a, out, sizeof(out), &lost) == 123 && lost == 0);
    assert(memcmp(out, source, 123) == 0);
    h.session_start = h.total;
    b = 0;
    assert(rx_history_read(&h, &b, out, sizeof(out), &lost) == 0 && b == h.total);
    rx_history_append(&h, source, 16);
    assert(rx_history_read(&h, &b, out, sizeof(out), &lost) == 16 && lost == 0);
    assert(memcmp(out, source, 16) == 0);
    puts("PASS: independent cursors, binary bytes, wrap, overwrite, reboot, empty reads, content isolation");
    return 0;
}
