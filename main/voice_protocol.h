#pragma once
#include <stddef.h>
#include <stdint.h>
#include "ap580_protocol.h"

#define VOICE_SYNC_WORD 0xDDAA96E7U
#define VOICE_FRAME_BYTES 16U
#define VOICE_MAX_RECORDS 8U
#define VOICE_PACKET_MAX 103U

typedef struct { uint8_t data[VOICE_PACKET_MAX]; size_t len; } voice_packet_t;
typedef void (*voice_packet_cb_t)(const voice_packet_t *packet, void *user);
typedef void (*voice_frames_cb_t)(const uint8_t *frames, unsigned count, uint32_t rate, void *user);

typedef struct {
    uint8_t payload[98], state, sync, descriptor;
    size_t used, expected;
    uint32_t packets, frames, crc_errors, bad_descriptors, sync_drops;
} voice_wire_rx_t;

typedef struct {
    uint8_t uart[16], frames[VOICE_MAX_RECORDS * VOICE_FRAME_BYTES];
    unsigned used, count;
    uint32_t rate, first_ms, last_ms;
    uint32_t accepted, bad_length, non_voice, sync_drops;
} voice_uart_rx_t;

unsigned voice_payload_bytes(uint32_t rate);
size_t voice_encode(uint32_t rate, const uint8_t *frames, unsigned count, voice_packet_t *packet);
void voice_wire_feed(voice_wire_rx_t *rx, const uint8_t *data, size_t len,
                     voice_frames_cb_t callback, void *user);
void voice_uart_feed(voice_uart_rx_t *rx, const uint8_t *data, size_t len,
                     uint32_t rate, uint32_t now, voice_packet_cb_t callback, void *user);
void voice_uart_flush_due(voice_uart_rx_t *rx, uint32_t now, voice_packet_cb_t callback, void *user);
