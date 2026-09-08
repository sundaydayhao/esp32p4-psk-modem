#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "modem_image.h"
#include "voice_protocol.h"

typedef enum { CONTENT_TEXT, CONTENT_IMAGE, CONTENT_AUDIO, CONTENT_COUNT } content_type_t;
typedef struct {
    uint32_t epoch, image_version, image_rows, image_bad_rows, image_completed;
    uint32_t voice_rate, voice_packets, voice_frames, voice_crc_errors, voice_resyncs;
    uint32_t uart_rx_frames, uart_bad_length, uart_tx_frames, uart_timeouts, queue_drops;
    uint32_t uart_rx_bytes, uart_sync_drops, uart_non_voice;
    uint32_t uart_rx_samples, uart_rx_low_samples;
    uint32_t uart_frame_errors, uart_parity_errors, uart_overflows;
    uint32_t voice_tx_packets, voice_tx_bytes, voice_tx_pending;
    uint8_t last_voice_frame[16];
    bool uart_ready;
} content_status_t;
typedef struct {
    voice_packet_t packet;
    size_t offset;
    uint32_t epoch;
} voice_tx_stream_t;

const char *content_name(content_type_t type);
esp_err_t content_service_init(void);
/* The modem/control task owns begin and receive. UART owns its driver and parser. */
void content_begin(content_type_t type, bool transmit, bool active, uint32_t rate, uint32_t epoch);
bool content_receive(const uint8_t *data, size_t len); /* true requests binary sync reacquisition */
void content_get_status(content_status_t *status);
void content_dump_uart_diagnostics(void);
void content_copy_image(uint8_t *pixels, content_status_t *status);
int content_voice_tx_fetch(void *stream);
