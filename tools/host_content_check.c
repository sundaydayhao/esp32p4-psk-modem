/* Real P4 image/AP580 protocol and PSK paths; finite deterministic ideal channel. */
#include "modem_image.h"
#include "voice_protocol.h"
#include "psk_rx.h"
#include "psk_tx.h"
#include "psk_codec.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t pixels[9216], expected[4096], stream[65536];
static size_t stream_len, stream_pos, received;
static unsigned seen_packets;
static uint32_t expected_rate;
static voice_packet_t last_packet;
static modem_image_rx_parser_t image_rx;
static voice_wire_rx_t wire_rx;
static modem_image_tx_stream_t image_tx;
static psk_tx_t tx;
static psk_rx_t rx;

static void row_cb(uint16_t row, const uint8_t *data, size_t len, uint16_t complete, void *user)
{
    (void)complete; (void)user;
    assert(row < 96 && len == 96);
    memcpy(pixels + row * 96, data, len);
}
static void frames_cb(const uint8_t *frames, unsigned count, uint32_t rate, void *user)
{
    (void)user;
    assert(rate == expected_rate && received + count * 16 <= sizeof(expected));
    assert(!memcmp(frames, expected + received, count * 16));
    received += count * 16;
}
static void packet_cb(const voice_packet_t *packet, void *user)
{
    (void)user; ++seen_packets; last_packet = *packet;
}
static void make_frames(uint32_t rate, unsigned count)
{
    expected_rate = rate; received = 0;
    memset(expected, 0, sizeof(expected));
    uint32_t random = 0x713c92a5;
    for (unsigned i = 0; i < count; ++i) {
        uint8_t *f = expected + i * 16;
        f[0] = 0x59; f[1] = 0x4f; f[2] = 1; f[3] = voice_payload_bytes(rate);
        for (unsigned j = 4; j < 4U + f[3]; ++j) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            f[j] = (uint8_t)random;
        }
        f[15] = (uint8_t)(i * 37); /* opaque trailer, including zero and high bits */
        if (i % 3 == 0) memcpy(f + 4, "\xdd\xaa\x96\xe7", 4);
        if (i % 3 == 1) { f[4] = 0; f[5] = 0xff; }
    }
}
static void image_protocol(void)
{
    uint8_t zero[96] = {0};
    assert(modem_image_row_crc(0, zero, 96) == 0x1cc2);
    modem_image_tx_stream_reset(&image_tx);
    for (size_t i = 0; i < 19514; ++i) stream[i] = modem_image_tx_stream_fetch(&image_tx);
    assert(!memcmp(stream, "~IMG:96:96:RGB332\n", 18));
    uint32_t psk_bits = 0, mfsk_bits = 0;
    for (size_t i = 0; i < 19514 - MODEM_IMAGE_GAP_CHARS; ++i) {
        uint32_t code = psk_codec_varicode_code(stream[i]);
        while (code) { ++psk_bits; code >>= 1; }
        psk_bits += 2;
        code = psk_codec_mfsk_varicode_table()[stream[i]];
        while (code) { ++mfsk_bits; code >>= 1; }
    }
    printf("Image encoded bits excluding repeat gap: PSK=%lu MFSK=%lu\n", (unsigned long)psk_bits, (unsigned long)mfsk_bits);
    assert(psk_bits == MODEM_IMAGE_PSK_BITS && mfsk_bits == MODEM_IMAGE_MFSK_BITS);
    modem_image_rx_parser_init(&image_rx);
    /* Lose the header and corrupt row 17; all other rows must survive. */
    stream[18 + 17 * 203 + 5] ^= 1;
    for (size_t i = 18; i < 19514; ++i)
        modem_image_rx_parser_feed(&image_rx, (const char *)stream + i, 1, NULL, row_cb, NULL);
    assert(image_rx.valid_rows == 95 && image_rx.total_invalid_rows == 1 && image_rx.completed_frames == 0);
    stream[18 + 17 * 203 + 5] ^= 1;
    /* A repeated header retains incomplete rows; a valid repeat repairs the hole. */
    modem_image_rx_parser_feed(&image_rx, (const char *)stream, 18, NULL, row_cb, NULL);
    assert(image_rx.valid_rows == 95);
    modem_image_rx_parser_feed(&image_rx, (const char *)stream + 18 + 17 * 203, 203, NULL, row_cb, NULL);
    assert(image_rx.valid_rows == 96 && image_rx.completed_frames == 1);
    assert(!memcmp(pixels, g_modem_test_image_rgb332, sizeof(pixels)));
    puts("PASS image: HPM stream length, header loss, row CRC, repeat repair, all 9216 pixels");
}
static void voice_protocol(uint32_t rate)
{
    /* Independent published HPM vectors, not encoder/decoder self-consistency. */
    uint8_t golden[16] = {0x59, 0x4f, 1, 0};
    golden[3] = voice_payload_bytes(rate); golden[15] = 0xa5;
    for (unsigned i = 0; i < golden[3]; ++i) golden[4+i] = i;
    voice_packet_t vector;
    assert(voice_encode(rate, golden, 1, &vector));
    const unsigned golden_crc = rate == 600 ? 0x13b4 : rate == 1200 ? 0xe6a9 : 0xb448;
    assert(((unsigned)vector.data[vector.len-2] << 8 | vector.data[vector.len-1]) == golden_crc);
    const unsigned count = rate == 1200 ? 8 : 6;
    make_frames(rate, count);
    voice_packet_t packet;
    assert(voice_encode(rate, expected, count, &packet) == 7 + count * (voice_payload_bytes(rate) + 1));
    memset(&wire_rx, 0, sizeof(wire_rx));
    for (size_t i = 0; i < packet.len; ++i) voice_wire_feed(&wire_rx, packet.data + i, 1, frames_cb, NULL);
    assert(received == count * 16 && wire_rx.packets == 1 && wire_rx.crc_errors == 0);
    packet.data[7] ^= 0x40;
    voice_wire_feed(&wire_rx, packet.data, packet.len, frames_cb, NULL);
    assert(received == count * 16 && wire_rx.crc_errors == 1);
    packet.data[7] ^= 0x40; received = 0;
    const uint8_t invalid[] = {0xdd,0xaa,0x96,0xe7,0xff,0xdd,0xaa,0x96,0xe7,0x10};
    voice_wire_feed(&wire_rx, invalid, sizeof(invalid), frames_cb, NULL);
    voice_wire_feed(&wire_rx, packet.data, packet.len, frames_cb, NULL);
    assert(received == count * 16 && wire_rx.bad_descriptors == 2);
    voice_uart_rx_t uart = {0}; seen_packets = 0;
    for (unsigned i = 0; i < count * 16; ++i) voice_uart_feed(&uart, expected + i, 1, rate, 100 + i / 16, packet_cb, NULL);
    assert(uart.accepted == count && seen_packets == 1 && last_packet.len == packet.len);
    assert(!memcmp(last_packet.data, packet.data, packet.len));
    voice_uart_feed(&uart, expected, 16, rate, 200, packet_cb, NULL);
    voice_uart_flush_due(&uart, 499, packet_cb, NULL); assert(seen_packets == 1);
    voice_uart_flush_due(&uart, 500, packet_cb, NULL); assert(seen_packets == 2);
    uint8_t bad[16]; memcpy(bad, expected, 16); bad[3] = rate == 1200 ? 7 : 11;
    voice_uart_feed(&uart, bad, 16, rate, 600, packet_cb, NULL); assert(uart.bad_length == 1);
    printf("PASS AP580 %lu: fragmented UART/wire, opaque sync/NUL/FF/trailer, strict CRC, descriptor, flush\n", (unsigned long)rate);
}
static int fetch(void *user)
{
    (void)user;
    return stream_pos < stream_len ? stream[stream_pos++] : 0;
}
static size_t available_bytes;
static int live_fetch(void *user)
{
    (void)user;
    return stream_pos < available_bytes ? stream[stream_pos++] : -1;
}
static void setup(int binary)
{
    psk_tx_init(&tx, 48000);
    const psk_rx_config_t config = {.mode = PSK_MODE_2X8PSK1200F, .sample_rate_hz = 48000,
        .rx_tone_hz = 2000, .squelch = 20, .afc_enabled = 1, .squelch_enabled = 0};
    psk_rx_init(&rx, &config);
    psk_rx_set_binary_sync(&rx, binary ? VOICE_SYNC_WORD : 0);
}
static void voice_roundtrip(uint32_t rate)
{
    const unsigned group = rate == 1200 ? 8 : 6, count = group * 24;
    make_frames(rate, count);
    stream_pos = 0; stream_len = 768; memset(stream, 0, stream_len);
    for (unsigned i = 0; i < count; i += group) {
        voice_packet_t packet;
        assert(voice_encode(rate, expected + i * 16, group, &packet));
        memcpy(stream + stream_len, packet.data, packet.len); stream_len += packet.len;
    }
    memset(&wire_rx, 0, sizeof(wire_rx)); setup(1);
    assert(!psk_tx_start_binary_stream(&tx, PSK_MODE_2X8PSK1200F, 2000, fetch, NULL));
    uint32_t samples = 0;
    for (; samples < 48000 * 30 && received < count * 16; samples += 256) {
        int16_t pcm[256]; char out[256];
        assert(psk_tx_generate(&tx, pcm, 256) == 256);
        const size_t n = psk_rx_process(&rx, pcm, 256, out, sizeof(out));
        voice_wire_feed(&wire_rx, (const uint8_t *)out, n, frames_cb, NULL);
    }
    assert(received == count * 16 && wire_rx.packets == 24 && wire_rx.crc_errors == 0);
    printf("PASS PSK binary %lu: %u distinct AP580 frames / %zu bytes exact, 24 CRC packets, %.3f audio seconds\n",
        (unsigned long)rate, count, received, samples / 48000.0);
}
static void image_roundtrip(void)
{
    setup(0); memset(pixels, 0, sizeof(pixels));
    modem_image_rx_parser_init(&image_rx); modem_image_tx_stream_reset(&image_tx);
    assert(!psk_tx_start_stream(&tx, PSK_MODE_2X8PSK1200F, 2000, modem_image_tx_stream_fetch, &image_tx));
    uint32_t samples = 0;
    for (; samples < 48000 * 100 && !image_rx.completed_frames; samples += 256) {
        int16_t pcm[256]; char out[256];
        assert(psk_tx_generate(&tx, pcm, 256) == 256);
        const size_t n = psk_rx_process(&rx, pcm, 256, out, sizeof(out));
        modem_image_rx_parser_feed(&image_rx, out, n, NULL, row_cb, NULL);
    }
    assert(image_rx.completed_frames && !memcmp(pixels, g_modem_test_image_rgb332, sizeof(pixels)));
    printf("PASS PSK image: all 96 CRC rows / 9216 pixels exact, bad rows %lu, %.3f audio seconds\n",
        (unsigned long)image_rx.total_invalid_rows, samples / 48000.0);
}
static void voice_idle_resume(uint32_t rate)
{
    const unsigned group = rate == 1200 ? 8 : 6, count = group * 4;
    make_frames(rate, count);
    stream_pos = stream_len = available_bytes = 0;
    size_t first_burst = 0;
    for (unsigned i = 0; i < count; i += group) {
        voice_packet_t packet;
        assert(voice_encode(rate, expected + i * 16, group, &packet));
        memcpy(stream + stream_len, packet.data, packet.len); stream_len += packet.len;
        if (i == group) first_burst = stream_len;
    }
    memset(&wire_rx, 0, sizeof(wire_rx)); setup(1);
    assert(!psk_tx_start_binary_stream(&tx, PSK_MODE_2X8PSK1200F, 2000, live_fetch, NULL));
    for (uint32_t samples = 0; samples < 48000 * 10; samples += 256) {
        available_bytes = samples < 48000 ? 0 : samples < 48000 * 6 ? first_burst : stream_len;
        int16_t pcm[256]; char out[256];
        assert(psk_tx_generate(&tx, pcm, 256) == 256 && psk_tx_active(&tx));
        const size_t n = psk_rx_process(&rx, pcm, 256, out, sizeof(out));
        voice_wire_feed(&wire_rx, (const uint8_t *)out, n, frames_cb, NULL);
        if (samples < 48000) assert(received == 0); /* idle isn't a voice frame */
        if (samples >= 48000 * 5 && samples < 48000 * 6)
            assert(received == group * 2 * VOICE_FRAME_BYTES);
    }
    assert(received == count * VOICE_FRAME_BYTES && wire_rx.packets == 4 && wire_rx.crc_errors == 0);
    printf("PASS continuous AP580 %lu: empty startup, two bursts, idle gap, exact frames, TX remains active\n",
           (unsigned long)rate);
}
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    image_protocol();
    const uint32_t rates[] = {600, 1200, 2400};
    for (unsigned i = 0; i < 3; ++i) voice_protocol(rates[i]);
    for (unsigned i = 0; i < 3; ++i) voice_roundtrip(rates[i]);
    for (unsigned i = 0; i < 3; ++i) voice_idle_resume(rates[i]);
    image_roundtrip();
    puts("PASS: finite ideal-channel validation; physical link BER and audible speech need hardware tests");
    return 0;
}
