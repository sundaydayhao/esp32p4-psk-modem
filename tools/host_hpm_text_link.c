/* Link P4 TX with the unchanged HPM host RX sources. No board or file in the
 * HPM tree is modified. 15 s acquisition + a complete 30 s repeated window.
 * Use p4_text_tx_check/joint_psk_check to score the saved decoded bytes. */
#include "psk_rx.h"
#include "psk_tx.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static psk_tx_t tx;
static psk_rx_t rx;
static char output[64000];
int main(int argc, char **argv) {
    if (argc != 7) {
        fprintf(stderr, "usage: %s MODE TONE SQL SCALE EXPECTED_FILE OUTPUT_FILE\n", argv[0]);
        return 2;
    }
    psk_mode_t mode = PSK_MODE_COUNT;
    for (int i = 0; i < PSK_MODE_COUNT; ++i)
        if (!strcmp(psk_mode_name(i), argv[1])) mode = i;
    if (mode == PSK_MODE_COUNT) return 2;
    char expected[256] = {0};
    FILE *f = fopen(argv[5], "rb");
    if (!f) return 2;
    size_t length = fread(expected, 1, sizeof(expected)-1, f);
    int extra = fgetc(f); fclose(f);
    if (!length || extra != EOF || memchr(expected, 0, length) || !strcmp(argv[5], argv[6])) return 2;
    int tone = atoi(argv[2]), sql = atoi(argv[3]);
    float scale = (float)atof(argv[4]);
    if (tone < 100 || tone > 3500 || (sql != 0 && sql != 1) ||
        !isfinite(scale) || scale <= 0.0f || scale > 1.0f) return 2;
    psk_tx_init(&tx, 48000);
    const psk_rx_config_t cfg = {.mode=mode, .sample_rate_hz=48000,
        .rx_tone_hz=tone, .squelch=35, .afc_enabled=1, .squelch_enabled=sql};
    psk_rx_init(&rx, &cfg);
    if (psk_tx_start_text_loop(&tx, mode, tone, expected)) return 2;
    size_t used=0, closed=0;
    for (int sample=0; sample<45*48000; sample+=128) {
        int16_t pcm[128]; char text[128];
        size_t n=psk_tx_generate(&tx, pcm, 128);
        if (n != 128) return 2;
        for (size_t i=0;i<n;++i) pcm[i]=(int16_t)(pcm[i]*scale);
        size_t got=psk_rx_process(&rx, pcm, n, text, sizeof(text));
        if (sample>=15*48000) {
            if (used+got>=sizeof(output)) return 2;
            memcpy(output+used, text, got); used+=got;
            closed+=!rx.rx.sq_open;
        }
    }
    f=fopen(argv[6],"wb"); if(!f)return 2;
    size_t written=fwrite(output,1,used,f);
    int close_result=fclose(f);
    if (written != used || close_result != 0) return 2;
    printf("mode=%s tone=%d sql=%d scale=%f received=%zu closed_blocks=%zu\n",argv[1],tone,sql,scale,used,closed);
    return 0;
}
