/* HPM6750 c84c683 AP580 wire format, extracted from core1/src/main.c. */
#include "voice_protocol.h"
#include <string.h>

static unsigned rate_code(uint32_t rate)
{
    return rate == 600 ? 1 : rate == 1200 ? 2 : rate == 2400 ? 3 : 0;
}
static uint32_t code_rate(unsigned code)
{
    static const uint32_t rates[] = {0, 600, 1200, 2400};
    return code < 4 ? rates[code] : 0;
}
unsigned voice_payload_bytes(uint32_t rate) { return rate == 1200 ? 11 : 7; }
static unsigned group_limit(uint32_t rate) { return rate == 1200 ? 8 : 6; }
static uint16_t crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xffff;
    while (len--) {
        crc ^= (uint16_t)*data++ << 8;
        for (unsigned b = 0; b < 8; ++b)
            crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    return crc;
}

size_t voice_encode(uint32_t rate, const uint8_t *frames, unsigned count, voice_packet_t *packet)
{
    if (!frames || !packet || (rate != 0 && !rate_code(rate)) ||
        !count || count > group_limit(rate)) return 0;
    const unsigned payload = voice_payload_bytes(rate);
    packet->data[0] = 0xdd; packet->data[1] = 0xaa;
    packet->data[2] = 0x96; packet->data[3] = 0xe7;
    packet->data[4] = (uint8_t)((rate_code(rate) << 4) | count);
    size_t pos = 5;
    for (unsigned i = 0; i < count; ++i) {
        const uint8_t *frame = frames + i * VOICE_FRAME_BYTES;
        if (frame[0] != 0x59 || frame[1] != 0x4f || frame[2] != 1 || frame[3] != payload)
            return 0;
        memcpy(packet->data + pos, frame + 4, payload);
        pos += payload;
        /* Preserve the opaque AP580 trailer; it is not our transport CRC. */
        packet->data[pos++] = frame[15];
    }
    const uint16_t crc = crc16(packet->data + 4, pos - 4);
    packet->data[pos++] = (uint8_t)(crc >> 8);
    packet->data[pos++] = (uint8_t)crc;
    packet->len = pos;
    return pos;
}

void voice_wire_feed(voice_wire_rx_t *rx, const uint8_t *data, size_t len,
                     voice_frames_cb_t callback, void *user)
{
    static const uint8_t sync[] = {0xdd, 0xaa, 0x96, 0xe7};
    for (size_t i = 0; i < len; ++i) {
        const uint8_t byte = data[i];
        if (rx->state == 0) {
            rx->sync = byte == sync[rx->sync] ? rx->sync + 1 : byte == sync[0] ? 1 : 0;
            if (rx->sync == 4) { rx->sync = 0; rx->state = 1; }
            else if (!rx->sync && byte) ++rx->sync_drops; /* Zero is normal idle. */
            continue;
        }
        if (rx->state == 1) {
            const unsigned code = byte >> 4, count = byte & 15;
            if (code > 3 || !count || count > group_limit(code_rate(code))) {
                ++rx->bad_descriptors;
                rx->state = 0;
                rx->sync = byte == sync[0] ? 1 : 0;
                continue;
            }
            rx->descriptor = byte;
            rx->used = 0;
            rx->expected = count * (voice_payload_bytes(code_rate(code)) + 1) + 2;
            rx->state = 2;
            continue;
        }
        /* Never search for sync inside a length-delimited opaque payload. */
        rx->payload[rx->used++] = byte;
        if (rx->used != rx->expected) continue;
        uint8_t checked[97];
        checked[0] = rx->descriptor;
        memcpy(checked + 1, rx->payload, rx->expected - 2);
        const uint16_t received = ((uint16_t)rx->payload[rx->expected - 2] << 8) |
                                  rx->payload[rx->expected - 1];
        if (received == crc16(checked, rx->expected - 1)) {
            const uint32_t rate = code_rate(rx->descriptor >> 4);
            const unsigned count = rx->descriptor & 15, payload = voice_payload_bytes(rate);
            uint8_t frames[VOICE_MAX_RECORDS * VOICE_FRAME_BYTES] = {0};
            for (unsigned j = 0; j < count; ++j) {
                uint8_t *frame = frames + j * VOICE_FRAME_BYTES;
                frame[0] = 0x59; frame[1] = 0x4f; frame[2] = 1; frame[3] = payload;
                memcpy(frame + 4, rx->payload + j * (payload + 1), payload);
                frame[15] = rx->payload[j * (payload + 1) + payload];
            }
            ++rx->packets; rx->frames += count;
            if (callback) callback(frames, count, rate, user);
        } else ++rx->crc_errors; /* HPM's current default: discard corrupt speech. */
        rx->state = 0; rx->used = 0;
    }
}

static void flush_group(voice_uart_rx_t *rx, voice_packet_cb_t callback, void *user)
{
    voice_packet_t packet;
    if (rx->count && voice_encode(rx->rate, rx->frames, rx->count, &packet) && callback)
        callback(&packet, user);
    rx->count = 0;
}

void voice_uart_feed(voice_uart_rx_t *rx, const uint8_t *data, size_t len,
                     uint32_t rate, uint32_t now, voice_packet_cb_t callback, void *user)
{
    for (size_t i = 0; i < len; ++i) {
        const uint8_t byte = data[i];
        if (!rx->used && byte != 0x59) { ++rx->sync_drops; continue; }
        if (rx->used == 1 && byte != 0x4f) {
            ++rx->sync_drops; rx->used = byte == 0x59 ? 1 : 0; continue;
        }
        rx->uart[rx->used++] = byte;
        if (rx->used != VOICE_FRAME_BYTES) continue;
        rx->used = 0;
        if (rx->uart[2] != 1) { ++rx->non_voice; continue; }
        if (rx->uart[3] != voice_payload_bytes(rate)) { ++rx->bad_length; continue; }
        if (rx->count && rx->rate != rate) flush_group(rx, callback, user);
        if (!rx->count) { rx->rate = rate; rx->first_ms = now; }
        memcpy(rx->frames + rx->count * VOICE_FRAME_BYTES, rx->uart, VOICE_FRAME_BYTES);
        ++rx->count; ++rx->accepted; rx->last_ms = now;
        if (rx->count == group_limit(rate)) flush_group(rx, callback, user);
    }
}

void voice_uart_flush_due(voice_uart_rx_t *rx, uint32_t now, voice_packet_cb_t callback, void *user)
{
    if (rx->count && ((uint32_t)(now - rx->last_ms) >= 300 ||
                      (uint32_t)(now - rx->first_ms) >= 760)) flush_group(rx, callback, user);
}
