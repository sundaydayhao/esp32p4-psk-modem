#include "rx_history.h"
#include <string.h>

void rx_history_append(rx_history_t *h, const uint8_t *data, size_t len)
{
    if (!data || !len) return;
    if (len > RX_HISTORY_CAPACITY) {
        const size_t skip = len - RX_HISTORY_CAPACITY;
        h->total += skip;
        data += skip;
        len -= skip;
    }
    const size_t pos = h->total % RX_HISTORY_CAPACITY;
    const size_t first = len < RX_HISTORY_CAPACITY - pos ? len : RX_HISTORY_CAPACITY - pos;
    memcpy(h->bytes + pos, data, first);
    memcpy(h->bytes, data + first, len - first);
    h->total += len;
}

size_t rx_history_read(const rx_history_t *h, uint64_t *cursor,
                       uint8_t *out, size_t capacity, uint64_t *lost)
{
    uint64_t oldest = h->total > RX_HISTORY_CAPACITY ? h->total - RX_HISTORY_CAPACITY : 0;
    if (oldest < h->session_start) oldest = h->session_start;
    *lost = 0;
    if (*cursor > h->total) *cursor = oldest;
    if (*cursor < oldest) {
        *lost = oldest - *cursor;
        *cursor = oldest;
    }
    size_t len = (size_t)(h->total - *cursor);
    if (len > capacity) len = capacity;
    const size_t pos = *cursor % RX_HISTORY_CAPACITY;
    const size_t first = len < RX_HISTORY_CAPACITY - pos ? len : RX_HISTORY_CAPACITY - pos;
    memcpy(out, h->bytes + pos, first);
    memcpy(out + first, h->bytes, len - first);
    *cursor += len;
    return len;
}
