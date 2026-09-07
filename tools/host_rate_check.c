/* One non-repeating, fully checked payload. Host timing is NOT P4 timing. */
#include "audio_decim.h"
#include "psk_rx.h"
#include "psk_tx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PAYLOAD_BYTES
#define PAYLOAD_BYTES 4096
#endif
#define BLOCK 256
#define SAMPLE_RATE 48000
static const char begin[] = "<BEGIN-P4-RATE-7E31>\n";
static const char end[] = "\n<END-P4-RATE-C925>";
static char stream[8192], expected[5000], decoded[20000];
static size_t stream_pos;
static int fetch(void *arg)
{
    (void)arg;
    if (stream_pos < strlen(stream)) return (unsigned char)stream[stream_pos++];
    return ' '; /* clock the complete interleaver and traceback out */
}

static int check(psk_mode_t mode, int offset_hz, int drop_symbols)
{
    const psk_mode_info_t *info = psk_mode_info(mode);
    const double rate = info->baud * info->carriers * info->bits_per_symbol *
        (info->fec ? (info->punctured ? 2.0/3.0 : 0.5) : 1.0);
    const int center_hz = mode == PSK_MODE_2X8PSK1200F ? 2000 : 1500;
    stream_pos = 0;
    memset(decoded, 0, sizeof(decoded));
    psk_tx_t tx;
    psk_rx_t rx;
    audio_decim_state_t decim;
    audio_decim_init(&decim);
    psk_tx_init(&tx, SAMPLE_RATE);
    const psk_rx_config_t config = {.mode = mode, .sample_rate_hz = SAMPLE_RATE,
        .rx_tone_hz = center_hz, .squelch = 20, .afc_enabled = 1, .squelch_enabled = 0};
    psk_rx_init(&rx, &config);
    if (psk_tx_start_stream(&tx, mode, center_hz + offset_hz, fetch, NULL)) return 1;
    size_t used = 0;
    const uint64_t limit = (uint64_t)((strlen(stream) * 16.0 / rate + 20.0) * SAMPLE_RATE);
    uint64_t samples = 0;
    int overflow = 0;
    size_t drop_left = 0;
    int drop_started = 0;
    for (; samples < limit; samples += BLOCK) {
        int16_t pcm[BLOCK], filtered[BLOCK];
        char chunk[256];
        const size_t n = psk_tx_generate(&tx, pcm, BLOCK);
        if (n != BLOCK) return 1;
        if (drop_symbols > 0 && !drop_started && samples >= 10U * SAMPLE_RATE) {
            drop_left = (size_t)(drop_symbols * SAMPLE_RATE / info->baud + 0.5f);
            drop_started = 1;
        }
        const size_t skip = drop_left < n ? drop_left : n;
        drop_left -= skip;
        const size_t count = audio_decim_process(&decim, pcm + skip, n - skip, filtered, 1);
        const size_t len = psk_rx_process(&rx, filtered, count, chunk, sizeof(chunk));
        if (used + len >= sizeof(decoded)) { overflow = 1; break; }
        memcpy(decoded + used, chunk, len);
        used += len;
        decoded[used] = 0;
        if (strstr(decoded, end)) break;
    }
    const char *start = strstr(decoded, begin);
    const char *finish = start ? strstr(start, end) : NULL;
    const size_t wanted = strlen(expected);
    const size_t received = finish ? (size_t)(finish + strlen(end) - start) : 0;
    size_t mismatches = 0;
    if (start && finish) {
        size_t common = received < wanted ? received : wanted;
        for (size_t i = 0; i < common; ++i) mismatches += start[i] != expected[i];
        mismatches += received > wanted ? received - wanted : wanted - received;
    }
    const int ok = !overflow && start && finish && received == wanted && mismatches == 0;
    printf("%s %s center=%d offset=%d baud=%.3f carriers=%u information_bps=%.3f payload=%d framed_expected=%zu framed_received=%zu mismatches=%zu complete=%d overflow=%d audio_s=%.3f\n",
        ok ? "PASS" : "FAIL", info->name, center_hz, offset_hz, info->baud, info->carriers, rate,
        PAYLOAD_BYTES, wanted, received, mismatches, start && finish ? 1 : 0, overflow,
        (double)samples / SAMPLE_RATE);
    if (drop_symbols) printf("injected_symbol_drops=%d fec_reacquires=%lu\n", drop_symbols,
                             (unsigned long)rx.rx.debug_fec_reacquires);
    if (!ok) {
        psk_rx_debug_t debug; psk_rx_debug_snapshot(&rx, &debug);
        printf("failure detail: lock=%u branch=%u metrics=%d/%d pairs=%lu/%lu received=%zu\n",
            debug.lock_state,debug.fec_locked_branch,debug.fec_metric[0],debug.fec_metric[1],
            (unsigned long)debug.fec_pairs[0],(unsigned long)debug.fec_pairs[1],used);
        const char *dump = getenv("PSK_FAILED_OUTPUT");
        if (dump) { FILE *f=fopen(dump,"wb"); if(f){fwrite(decoded,1,used,f);fclose(f);} }
    }
    return !ok;
}

int main(int argc, char **argv)
{
    strcpy(expected, begin);
    size_t pos = strlen(expected);
    uint32_t state = 0x713c92a5U;
    for (int i = 0; i < PAYLOAD_BYTES; ++i) {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        expected[pos++] = (i % 64 == 63) ? '\n' : (char)(' ' + state % 95);
    }
    strcpy(expected + pos, end);
    const int drop_symbols = argc > 3 ? atoi(argv[3]) : 0;
    for (int i = 0; i < (drop_symbols ? 128 : 64); ++i) strcat(stream, "training ");
    strcat(stream, expected);
    int failures = 0;
    int cases = 0;
    const psk_mode_t modes[] = {PSK_MODE_BPSK1000, PSK_MODE_2XPSK1000,
        PSK_MODE_8PSK1000, PSK_MODE_8PSK1200F, PSK_MODE_2X8PSK1200F,
        PSK_MODE_8PSK125F, PSK_MODE_8PSK250F};
    const int offset = argc > 2 ? atoi(argv[2]) : 0;
    for (size_t i = 0; i < sizeof(modes)/sizeof(modes[0]); ++i) {
        /* Long K16 runs are explicit; the default suite remains the rate check. */
        if (argc == 1 && (modes[i] == PSK_MODE_8PSK125F || modes[i] == PSK_MODE_8PSK250F)) continue;
        if (argc > 1 && strcmp(argv[1], psk_mode_name(modes[i]))) continue;
        ++cases;
        ++failures; /* also fail if a case aborts internally */
        if (check(modes[i], offset, drop_symbols) == 0) --failures;
    }
    printf("summary: failures=%d; finite ideal-channel payload check, not measured hardware BER\n", failures);
    return !cases || failures ? 1 : 0;
}
