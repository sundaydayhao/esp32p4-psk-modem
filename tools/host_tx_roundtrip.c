#include "audio_decim.h"
#include "psk_rx.h"
#include "psk_tx.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOST_CODEC_RATE_HZ 48000
#define HOST_DECODE_RATE_HZ 16000
#define HOST_DECIMATION 3
#define HOST_TONE_HZ 1500
#define TX_CHUNK 512
#define DECODE_BLOCK 128
#define MAX_CODEC_SAMPLES (HOST_CODEC_RATE_HZ * 40)
#define MAX_DECODE_SAMPLES (HOST_DECODE_RATE_HZ * 40)
#define MAX_DECODED_TEXT 8192

static void append_text(char *dst, size_t cap, const char *src, size_t len)
{
    size_t used = strlen(dst);
    if (used + 1U >= cap) {
        return;
    }
    size_t n = len;
    if (used + n + 1U > cap) {
        n = cap - used - 1U;
    }
    memcpy(dst + used, src, n);
    dst[used + n] = '\0';
}

static int contains_fragment(const char *decoded, const char *expected)
{
    const size_t expected_len = strlen(expected);
    for (size_t size = expected_len > 40U ? 40U : expected_len; size >= 12U; --size) {
        for (size_t start = 0U; start + size <= expected_len; start += size / 2U ? size / 2U : 1U) {
            char fragment[64];
            memcpy(fragment, expected + start, size);
            fragment[size] = '\0';
            if (strstr(decoded, fragment)) {
                return 1;
            }
        }
        if (size == 12U) {
            break;
        }
    }
    return 0;
}

static int run_mode(psk_mode_t mode, const char *text)
{
    const psk_mode_info_t *info = psk_mode_info(mode);
    int16_t *codec = (int16_t *)malloc(MAX_CODEC_SAMPLES * sizeof(int16_t));
    int16_t *decode = (int16_t *)malloc(MAX_DECODE_SAMPLES * sizeof(int16_t));
    char *decoded = (char *)calloc(MAX_DECODED_TEXT, 1U);
    if (!codec || !decode || !decoded) {
        free(codec);
        free(decode);
        free(decoded);
        return 2;
    }

    psk_tx_t tx;
    psk_tx_init(&tx, HOST_CODEC_RATE_HZ);
    int rc = psk_tx_start_text(&tx, mode, HOST_TONE_HZ, text);
    if (rc != 0) {
        printf("FAIL %-11s tx_start=%d\n", info->name, rc);
        free(codec);
        free(decode);
        free(decoded);
        return 1;
    }

    size_t codec_count = 0U;
    while (psk_tx_active(&tx) && codec_count < MAX_CODEC_SAMPLES) {
        size_t n = psk_tx_generate(&tx, codec + codec_count, TX_CHUNK);
        codec_count += n;
        if (n == 0U) {
            break;
        }
    }

    audio_decim_state_t decim;
    audio_decim_init(&decim);
    size_t decode_count = audio_decim_process(&decim, codec, codec_count, decode, HOST_DECIMATION);

    const psk_rx_config_t cfg = {
        .mode = mode,
        .sample_rate_hz = HOST_DECODE_RATE_HZ,
        .rx_tone_hz = HOST_TONE_HZ,
        .squelch = 20,
        .afc_enabled = 1,
        .squelch_enabled = 0,
    };
    psk_rx_t modem;
    psk_rx_init(&modem, &cfg);

    for (size_t pos = 0U; pos < decode_count; pos += DECODE_BLOCK) {
        size_t n = decode_count - pos;
        if (n > DECODE_BLOCK) {
            n = DECODE_BLOCK;
        }
        char chunk[256];
        size_t out_len = psk_rx_process(&modem, decode + pos, n, chunk, sizeof(chunk));
        if (out_len > 0U) {
            append_text(decoded, MAX_DECODED_TEXT, chunk, out_len);
        }
    }

    const int ok = contains_fragment(decoded, text);
    psk_rx_debug_t debug;
    psk_rx_debug_snapshot(&modem, &debug);
    printf("%s %-11s tx=%zu dec=%zu decoded=%zu chars=%lu lock=%u sq=%d/%d frame=%lu/%lu/%lu\n",
           ok ? "PASS" : "FAIL", info->name, codec_count, decode_count, strlen(decoded),
           (unsigned long)debug.chars, debug.lock_state, debug.sq_level, debug.sq_thresh,
           (unsigned long)debug.valid_frames, (unsigned long)debug.invalid_frames,
           (unsigned long)debug.frame_sync_events);
    if (!ok && decoded[0] != '\0') {
        printf("  preview: %.160s\n", decoded);
    }

    free(codec);
    free(decode);
    free(decoded);
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    const char *text = argc > 1 ? argv[1] :
        "THE QUICK BROWN FOX 0123456789 THE QUICK BROWN FOX 0123456789 "
        "ESP32P4 PSK MODEM LOOPBACK HOST TEST END";
    int failures = 0;
    for (size_t i = 0U; i < psk_mode_count(); ++i) {
        const psk_mode_info_t *info = psk_mode_info_at(i);
        if (!info || !info->decode_supported) {
            continue;
        }
        if (run_mode(info->id, text) != 0) {
            ++failures;
        }
    }
    printf("summary: failures=%d\n", failures);
    return failures ? 1 : 0;
}
