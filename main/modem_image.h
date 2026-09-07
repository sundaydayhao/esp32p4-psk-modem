#pragma once

#include <stddef.h>
#include <stdint.h>
#include "psk_modes.h"

#define MODEM_IMAGE_WIDTH           96U
#define MODEM_IMAGE_HEIGHT          96U
#define MODEM_IMAGE_PIXEL_COUNT     (MODEM_IMAGE_WIDTH * MODEM_IMAGE_HEIGHT)
#define MODEM_IMAGE_ROW_HEX_CHARS   (MODEM_IMAGE_WIDTH * 2U)
#define MODEM_IMAGE_RX_LINE_CAPACITY 224U
#define MODEM_IMAGE_MAX_SECONDS     180U
#define MODEM_IMAGE_GAP_CHARS       8U
/* Exact encoded lengths of the current image, including row CRCs/header.
 * The HOST protocol test checks these against the actual stream and tables. */
#define MODEM_IMAGE_PSK_BITS        199077U
#define MODEM_IMAGE_MFSK_BITS       165639U

uint32_t modem_image_estimated_seconds(psk_mode_t mode);
int modem_image_mode_allowed(psk_mode_t mode);

extern const uint8_t g_modem_test_image_rgb332[MODEM_IMAGE_PIXEL_COUNT];

typedef enum {
    MODEM_IMAGE_TX_HEADER = 0,
    MODEM_IMAGE_TX_ROW_PREFIX,
    MODEM_IMAGE_TX_PIXELS,
    MODEM_IMAGE_TX_SEPARATOR,
    MODEM_IMAGE_TX_CRC,
    MODEM_IMAGE_TX_NEWLINE,
    MODEM_IMAGE_TX_HOLD,
} modem_image_tx_stage_t;

typedef struct {
    modem_image_tx_stage_t stage;
    uint16_t position;
    uint16_t row;
    uint16_t pixel;
    uint16_t row_crc;
    uint8_t nibble;
} modem_image_tx_stream_t;

typedef struct {
    char line[MODEM_IMAGE_RX_LINE_CAPACITY];
    uint16_t line_len;
    uint16_t valid_rows;
    uint16_t invalid_rows;
    uint32_t completed_frames;
    uint32_t total_valid_rows;
    uint32_t total_invalid_rows;
    uint32_t received_rows[3];
    uint8_t collecting;
    uint8_t frame_started;
    uint8_t frame_complete_reported;
    uint8_t row_pixels[MODEM_IMAGE_WIDTH];
} modem_image_rx_parser_t;

typedef void (*modem_image_frame_cb_t)(void *user);
typedef void (*modem_image_row_cb_t)(uint16_t row, const uint8_t *pixels,
                                     size_t count, uint16_t rows_complete,
                                     void *user);

void modem_image_tx_stream_reset(modem_image_tx_stream_t *stream);
int modem_image_tx_stream_fetch(void *user);
void modem_image_rx_parser_init(modem_image_rx_parser_t *parser);
void modem_image_rx_parser_feed(modem_image_rx_parser_t *parser,
                                const char *data, size_t length,
                                modem_image_frame_cb_t frame_cb,
                                modem_image_row_cb_t row_cb,
                                void *user);
uint16_t modem_image_row_crc(uint16_t row, const uint8_t *pixels, size_t count);
uint16_t modem_image_rgb332_to_rgb565(uint8_t pixel);
