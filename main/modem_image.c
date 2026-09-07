#include "modem_image.h"

#include <string.h>

static const char s_frame_header[] = "~IMG:96:96:RGB332";
static const char s_hex[] = "0123456789ABCDEF";

const uint8_t g_modem_test_image_rgb332[MODEM_IMAGE_PIXEL_COUNT] = {
#include "modem_image_pixels.inc"
};

uint32_t modem_image_estimated_seconds(psk_mode_t mode)
{
    if ((unsigned)mode >= PSK_MODE_COUNT) {
        return UINT32_MAX;
    }
    const psk_mode_info_t *info = psk_mode_info(mode);
    const float bits = info->mfsk_varicode ? MODEM_IMAGE_MFSK_BITS : MODEM_IMAGE_PSK_BITS;
    float rate = info->baud * info->bits_per_symbol * info->carriers;
    if (info->fec) {
        rate *= info->punctured ? (2.0f / 3.0f) : 0.5f;
    }
    /* Ten seconds cover acquisition and decoder buffering, not retransmits. */
    return (uint32_t)(bits / rate + 0.999f) + 10U;
}

int modem_image_mode_allowed(psk_mode_t mode)
{
    return modem_image_estimated_seconds(mode) <= MODEM_IMAGE_MAX_SECONDS;
}

static int hex_value(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    return -1;
}

static uint16_t crc16_update(uint16_t crc, uint8_t value)
{
    crc ^= (uint16_t)value << 8U;
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
        crc = (uint16_t)((crc << 1U) ^ ((crc & 0x8000U) ? 0x1021U : 0U));
    }
    return crc;
}

uint16_t modem_image_row_crc(uint16_t row, const uint8_t *pixels, size_t count)
{
    uint16_t crc = 0xffffU;
    crc = crc16_update(crc, (uint8_t)row);
    for (size_t i = 0U; pixels && i < count; ++i) {
        crc = crc16_update(crc, pixels[i]);
    }
    return crc;
}

uint16_t modem_image_rgb332_to_rgb565(uint8_t pixel)
{
    const uint16_t red3 = (uint16_t)((pixel >> 5U) & 0x07U);
    const uint16_t green3 = (uint16_t)((pixel >> 2U) & 0x07U);
    const uint16_t blue2 = (uint16_t)(pixel & 0x03U);
    const uint16_t red5 = (uint16_t)((red3 << 2U) | (red3 >> 1U));
    const uint16_t green6 = (uint16_t)((green3 << 3U) | green3);
    const uint16_t blue5 = (uint16_t)((blue2 << 3U) | (blue2 << 1U) | (blue2 >> 1U));
    return (uint16_t)((red5 << 11U) | (green6 << 5U) | blue5);
}

void modem_image_tx_stream_reset(modem_image_tx_stream_t *stream)
{
    if (!stream) {
        return;
    }
    memset(stream, 0, sizeof(*stream));
    stream->stage = MODEM_IMAGE_TX_HEADER;
}

int modem_image_tx_stream_fetch(void *user)
{
    modem_image_tx_stream_t *stream = (modem_image_tx_stream_t *)user;
    if (!stream) {
        return -1;
    }

    switch (stream->stage) {
    case MODEM_IMAGE_TX_HEADER:
        if (stream->position < sizeof(s_frame_header) - 1U) {
            return (unsigned char)s_frame_header[stream->position++];
        }
        stream->position = 0U;
        stream->row = 0U;
        stream->pixel = 0U;
        stream->nibble = 0U;
        stream->stage = MODEM_IMAGE_TX_ROW_PREFIX;
        return '\n';

    case MODEM_IMAGE_TX_ROW_PREFIX:
        if (stream->position == 0U) {
            stream->row_crc = modem_image_row_crc(
                stream->row,
                &g_modem_test_image_rgb332[(size_t)stream->row * MODEM_IMAGE_WIDTH],
                MODEM_IMAGE_WIDTH);
        }
        switch (stream->position++) {
        case 0U:
            return '~';
        case 1U:
            return 'I';
        case 2U:
            return s_hex[(stream->row >> 4U) & 0x0fU];
        case 3U:
            return s_hex[stream->row & 0x0fU];
        default:
            stream->position = 0U;
            stream->pixel = 0U;
            stream->nibble = 0U;
            stream->stage = MODEM_IMAGE_TX_PIXELS;
            return ':';
        }

    case MODEM_IMAGE_TX_PIXELS: {
        const uint8_t pixel = g_modem_test_image_rgb332[
            (size_t)stream->row * MODEM_IMAGE_WIDTH + stream->pixel];
        const int out = stream->nibble == 0U ? s_hex[pixel >> 4U] : s_hex[pixel & 0x0fU];
        stream->nibble ^= 1U;
        if (stream->nibble == 0U && ++stream->pixel >= MODEM_IMAGE_WIDTH) {
            stream->stage = MODEM_IMAGE_TX_SEPARATOR;
        }
        return out;
    }

    case MODEM_IMAGE_TX_SEPARATOR:
        stream->position = 0U;
        stream->stage = MODEM_IMAGE_TX_CRC;
        return ':';

    case MODEM_IMAGE_TX_CRC: {
        const uint8_t shift = (uint8_t)(12U - stream->position * 4U);
        const int out = s_hex[(stream->row_crc >> shift) & 0x0fU];
        if (++stream->position >= 4U) {
            stream->position = 0U;
            stream->stage = MODEM_IMAGE_TX_NEWLINE;
        }
        return out;
    }

    case MODEM_IMAGE_TX_NEWLINE:
        if (++stream->row >= MODEM_IMAGE_HEIGHT) {
            stream->row = 0U;
            stream->position = 0U;
            stream->stage = MODEM_IMAGE_TX_HOLD;
        } else {
            stream->stage = MODEM_IMAGE_TX_ROW_PREFIX;
        }
        return '\n';

    case MODEM_IMAGE_TX_HOLD:
        if (++stream->position >= MODEM_IMAGE_GAP_CHARS) {
            stream->position = 0U;
            stream->stage = MODEM_IMAGE_TX_HEADER;
        }
        return '\n';

    default:
        modem_image_tx_stream_reset(stream);
        return '\n';
    }
}

static int parse_hex_byte(const char *text, uint8_t *out)
{
    const int high = hex_value(text[0]);
    const int low = hex_value(text[1]);
    if (high < 0 || low < 0) {
        return 0;
    }
    *out = (uint8_t)((high << 4U) | low);
    return 1;
}

static int parse_hex16(const char *text, uint16_t *out)
{
    uint16_t value = 0U;
    for (uint8_t i = 0U; i < 4U; ++i) {
        const int digit = hex_value(text[i]);
        if (digit < 0) {
            return 0;
        }
        value = (uint16_t)((value << 4U) | (uint16_t)digit);
    }
    *out = value;
    return 1;
}

static void start_rx_frame(modem_image_rx_parser_t *parser,
                           modem_image_frame_cb_t frame_cb, void *user)
{
    parser->frame_started = 1U;
    parser->valid_rows = 0U;
    parser->invalid_rows = 0U;
    parser->frame_complete_reported = 0U;
    memset(parser->received_rows, 0, sizeof(parser->received_rows));
    if (frame_cb) {
        frame_cb(user);
    }
}

static void parse_line(modem_image_rx_parser_t *parser,
                       modem_image_frame_cb_t frame_cb,
                       modem_image_row_cb_t row_cb,
                       void *user)
{
    if (parser->line_len == sizeof(s_frame_header) - 1U &&
        memcmp(parser->line, s_frame_header, sizeof(s_frame_header) - 1U) == 0) {
        /* Image TX intentionally repeats complete frames. If the previous
         * cycle lost a row, the next header is a retransmission boundary, not
         * a reason to erase already verified rows and reset the UI progress. */
        if (!parser->frame_started || parser->frame_complete_reported) {
            start_rx_frame(parser, frame_cb, user);
        }
        return;
    }

    const size_t payload_offset = 5U;
    const size_t separator_offset = payload_offset + MODEM_IMAGE_ROW_HEX_CHARS;
    const size_t crc_offset = separator_offset + 1U;
    const size_t expected_length = crc_offset + 4U;
    if (parser->line_len != expected_length ||
        parser->line[0] != '~' || parser->line[1] != 'I' ||
        parser->line[4] != ':' || parser->line[separator_offset] != ':') {
        return;
    }

    uint8_t row = 0U;
    if (!parse_hex_byte(&parser->line[2], &row) || row >= MODEM_IMAGE_HEIGHT) {
        ++parser->invalid_rows;
        ++parser->total_invalid_rows;
        return;
    }
    for (size_t pixel = 0U; pixel < MODEM_IMAGE_WIDTH; ++pixel) {
        if (!parse_hex_byte(&parser->line[payload_offset + pixel * 2U],
                            &parser->row_pixels[pixel])) {
            ++parser->invalid_rows;
            ++parser->total_invalid_rows;
            return;
        }
    }
    uint16_t received_crc = 0U;
    if (!parse_hex16(&parser->line[crc_offset], &received_crc) ||
        received_crc != modem_image_row_crc(row, parser->row_pixels, MODEM_IMAGE_WIDTH)) {
        ++parser->invalid_rows;
        ++parser->total_invalid_rows;
        return;
    }

    /* The fixed row length, row number and CRC are sufficient to recover
     * when the frame header itself was lost on air. */
    if (!parser->frame_started ||
        (parser->frame_complete_reported && row == 0U)) {
        start_rx_frame(parser, frame_cb, user);
    }

    const uint32_t mask = 1UL << (row & 31U);
    uint32_t *word = &parser->received_rows[row >> 5U];
    if ((*word & mask) == 0U) {
        *word |= mask;
        ++parser->valid_rows;
        ++parser->total_valid_rows;
        if (parser->valid_rows == MODEM_IMAGE_HEIGHT &&
            !parser->frame_complete_reported) {
            parser->frame_complete_reported = 1U;
            ++parser->completed_frames;
        }
    }
    if (row_cb) {
        row_cb(row, parser->row_pixels, MODEM_IMAGE_WIDTH,
               parser->valid_rows, user);
    }
}

void modem_image_rx_parser_init(modem_image_rx_parser_t *parser)
{
    if (parser) {
        memset(parser, 0, sizeof(*parser));
    }
}

void modem_image_rx_parser_feed(modem_image_rx_parser_t *parser,
                                const char *data, size_t length,
                                modem_image_frame_cb_t frame_cb,
                                modem_image_row_cb_t row_cb,
                                void *user)
{
    if (!parser || !data) {
        return;
    }
    for (size_t i = 0U; i < length; ++i) {
        const char value = data[i];
        if (value == '~') {
            parser->line_len = 0U;
            parser->collecting = 1U;
            parser->line[parser->line_len++] = value;
            continue;
        }
        if (!parser->collecting || value == '\r') {
            continue;
        }
        if (value == '\n') {
            parse_line(parser, frame_cb, row_cb, user);
            parser->line_len = 0U;
            parser->collecting = 0U;
            continue;
        }
        if (parser->line_len + 1U >= sizeof(parser->line)) {
            parser->line_len = 0U;
            parser->collecting = 0U;
            continue;
        }
        parser->line[parser->line_len++] = value;
    }
}
