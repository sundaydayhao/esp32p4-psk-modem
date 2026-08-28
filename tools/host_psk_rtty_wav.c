#include "psk_rtty.h"
#include "psk_rx.h"
#include "audio_decim.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Mirror the firmware audio path: 48 kHz codec/audio-file input is filtered
 * and DC-blocked before the PSK decoder sees 48 kHz mono. */
#define HOST_CODEC_RATE_HZ 48000
#define HOST_DECODE_RATE_HZ 48000

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} text_buf_t;

typedef struct {
    text_buf_t text;
    size_t current_sample;
    size_t first_char_sample;
    int have_first_char;
} decode_ctx_t;

static void append_char(char c, void *userdata)
{
    decode_ctx_t *ctx = (decode_ctx_t *)userdata;
    text_buf_t *buf = &ctx->text;
    if (!ctx->have_first_char) {
        ctx->first_char_sample = ctx->current_sample;
        ctx->have_first_char = 1;
    }
    if (buf->len + 1 >= buf->cap) {
        size_t next = buf->cap ? buf->cap * 2 : 4096;
        char *p = (char *)realloc(buf->data, next);
        if (!p) {
            return;
        }
        buf->data = p;
        buf->cap = next;
    }
    buf->data[buf->len++] = c;
    buf->data[buf->len] = '\0';
}

static uint16_t read_u16(FILE *fp)
{
    uint8_t b[2];
    if (fread(b, 1, 2, fp) != 2) {
        return 0;
    }
    return (uint16_t)(b[0] | ((uint16_t)b[1] << 8));
}

static uint32_t read_u32(FILE *fp)
{
    uint8_t b[4];
    if (fread(b, 1, 4, fp) != 4) {
        return 0;
    }
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static int read_wav_mono16(const char *path, int16_t **samples, size_t *count, int *rate)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        perror(path);
        return 0;
    }

    char id[5] = {0};
    if (fread(id, 1, 4, fp) != 4 || memcmp(id, "RIFF", 4) != 0) {
        fclose(fp);
        return 0;
    }
    (void)read_u32(fp);
    if (fread(id, 1, 4, fp) != 4 || memcmp(id, "WAVE", 4) != 0) {
        fclose(fp);
        return 0;
    }

    uint16_t channels = 0;
    uint16_t bits = 0;
    uint32_t data_bytes = 0;
    long data_pos = 0;
    while (fread(id, 1, 4, fp) == 4) {
        uint32_t size = read_u32(fp);
        long next = ftell(fp) + (long)size + (long)(size & 1U);
        if (memcmp(id, "fmt ", 4) == 0) {
            uint16_t format = read_u16(fp);
            channels = read_u16(fp);
            *rate = (int)read_u32(fp);
            (void)read_u32(fp);
            (void)read_u16(fp);
            bits = read_u16(fp);
            if (format != 1) {
                fclose(fp);
                return 0;
            }
        } else if (memcmp(id, "data", 4) == 0) {
            data_pos = ftell(fp);
            data_bytes = size;
        }
        fseek(fp, next, SEEK_SET);
        if (channels && bits && data_pos) {
            break;
        }
    }

    if (!data_pos || channels < 1 || bits != 16 || *rate <= 0) {
        fclose(fp);
        return 0;
    }

    size_t frames = data_bytes / (size_t)(channels * 2U);
    int16_t *out = (int16_t *)malloc(frames * sizeof(int16_t));
    if (!out) {
        fclose(fp);
        return 0;
    }
    fseek(fp, data_pos, SEEK_SET);
    for (size_t i = 0; i < frames; ++i) {
        out[i] = (int16_t)read_u16(fp);
        for (uint16_t ch = 1; ch < channels; ++ch) {
            (void)read_u16(fp);
        }
    }
    fclose(fp);
    *samples = out;
    *count = frames;
    return 1;
}

static void apply_input_impairments(int16_t *samples, size_t count)
{
    const char *gain_text = getenv("PSK_INPUT_GAIN");
    const char *noise_text = getenv("PSK_NOISE_PEAK");
    const float gain = gain_text ? strtof(gain_text, NULL) : 1.0f;
    const int noise_peak = noise_text ? (int)strtol(noise_text, NULL, 10) : 0;
    uint32_t rng = 0x6d2b79f5U;

    if (gain == 1.0f && noise_peak <= 0) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        int noise = 0;
        if (noise_peak > 0) {
            rng = rng * 1664525U + 1013904223U;
            noise = (int)(((uint64_t)(rng >> 16U) * (uint32_t)(noise_peak * 2 + 1)) >> 16U) -
                    noise_peak;
        }
        int sample = (int)((float)samples[i] * gain) + noise;
        if (sample > 32767) {
            sample = 32767;
        } else if (sample < -32768) {
            sample = -32768;
        }
        samples[i] = (int16_t)sample;
    }
}

static int16_t clamp_i16_from_double(double v)
{
    if (v > 32767.0) {
        return 32767;
    }
    if (v < -32768.0) {
        return -32768;
    }
    return (int16_t)(v >= 0.0 ? v + 0.5 : v - 0.5);
}

static int resample_linear(const int16_t *in, size_t in_count, int in_rate,
                           int out_rate, int16_t **out, size_t *out_count)
{
    if (!in || !out || !out_count || in_count == 0U || in_rate <= 0 || out_rate <= 0) {
        return 0;
    }

    const size_t n = (size_t)((double)in_count * (double)out_rate /
                              (double)in_rate + 0.5);
    if (n == 0U) {
        return 0;
    }

    int16_t *resampled = (int16_t *)malloc(n * sizeof(int16_t));
    if (!resampled) {
        return 0;
    }

    for (size_t i = 0U; i < n; ++i) {
        const double src = (double)i * (double)in_rate / (double)out_rate;
        size_t base = (size_t)src;
        if (base >= in_count) {
            base = in_count - 1U;
        }
        const size_t next = base + 1U < in_count ? base + 1U : base;
        const double frac = src - (double)base;
        const double a = (double)in[base];
        const double b = (double)in[next];
        resampled[i] = clamp_i16_from_double(a + (b - a) * frac);
    }

    *out = resampled;
    *out_count = n;
    return 1;
}

static int host_decode_rate_hz(void)
{
    const char *text = getenv("PSK_HOST_DECODE_RATE_HZ");
    if (!text || !text[0]) {
        return HOST_DECODE_RATE_HZ;
    }
    const long rate = strtol(text, NULL, 10);
    if (rate <= 0 || HOST_CODEC_RATE_HZ % rate != 0) {
        fprintf(stderr, "invalid PSK_HOST_DECODE_RATE_HZ=%s\n", text);
        exit(2);
    }
    return (int)rate;
}

static psk_rtty_mode_t parse_mode(const char *name)
{
    if (strcmp(name, "BPSK31") == 0) {
        return PSK_RTTY_MODE_BPSK31;
    }
    if (strcmp(name, "BPSK63") == 0) {
        return PSK_RTTY_MODE_PSK63;
    }
    if (strcmp(name, "BPSK63F") == 0) {
        return PSK_RTTY_MODE_BPSK63F;
    }
    if (strcmp(name, "BPSK125") == 0) {
        return PSK_RTTY_MODE_BPSK125;
    }
    if (strcmp(name, "BPSK250") == 0) {
        return PSK_RTTY_MODE_BPSK250;
    }
    if (strcmp(name, "BPSK500") == 0) {
        return PSK_RTTY_MODE_BPSK500;
    }
    if (strcmp(name, "BPSK1000") == 0) {
        return PSK_RTTY_MODE_BPSK1000;
    }
    if (strcmp(name, "12xPSK125") == 0) {
        return PSK_RTTY_MODE_12XPSK125;
    }
    if (strcmp(name, "6xPSK250") == 0) {
        return PSK_RTTY_MODE_6XPSK250;
    }
    if (strcmp(name, "2xPSK500") == 0) {
        return PSK_RTTY_MODE_2XPSK500;
    }
    if (strcmp(name, "4xPSK500") == 0) {
        return PSK_RTTY_MODE_4XPSK500;
    }
    if (strcmp(name, "2xPSK800") == 0) {
        return PSK_RTTY_MODE_2XPSK800;
    }
    if (strcmp(name, "2xPSK1000") == 0) {
        return PSK_RTTY_MODE_2XPSK1000;
    }
    if (strcmp(name, "QPSK31") == 0) {
        return PSK_RTTY_MODE_QPSK31_USB;
    }
    if (strcmp(name, "QPSK63") == 0) {
        return PSK_RTTY_MODE_QPSK63;
    }
    if (strcmp(name, "QPSK125") == 0) {
        return PSK_RTTY_MODE_QPSK125;
    }
    if (strcmp(name, "QPSK250") == 0) {
        return PSK_RTTY_MODE_QPSK250;
    }
    if (strcmp(name, "QPSK500") == 0) {
        return PSK_RTTY_MODE_QPSK500;
    }
    if (strcmp(name, "8PSK125") == 0) {
        return PSK_RTTY_MODE_8PSK125;
    }
    if (strcmp(name, "8PSK250") == 0) {
        return PSK_RTTY_MODE_8PSK250;
    }
    if (strcmp(name, "8PSK500") == 0) {
        return PSK_RTTY_MODE_8PSK500;
    }
    if (strcmp(name, "8PSK1000") == 0) {
        return PSK_RTTY_MODE_8PSK1000;
    }
    if (strcmp(name, "8PSK125F") == 0) {
        return PSK_RTTY_MODE_8PSK125F;
    }
    if (strcmp(name, "8PSK250F") == 0) {
        return PSK_RTTY_MODE_8PSK250F;
    }
    if (strcmp(name, "8PSK500F") == 0) {
        return PSK_RTTY_MODE_8PSK500F;
    }
    if (strcmp(name, "8PSK1000F") == 0) {
        return PSK_RTTY_MODE_8PSK1000F;
    }
    if (strcmp(name, "8PSK1200F") == 0) {
        return PSK_RTTY_MODE_8PSK1200F;
    }
    fprintf(stderr, "unknown mode: %s\n", name);
    exit(2);
}

static psk_mode_t parse_app_mode(const char *name)
{
    for (size_t i = 0U; i < psk_mode_count(); ++i) {
        const psk_mode_info_t *info = psk_mode_info_at(i);
        if (info && strcmp(name, info->name) == 0) {
            return info->id;
        }
    }
    fprintf(stderr, "unknown application mode: %s\n", name);
    exit(2);
}

static char *clean_text(const char *s)
{
    size_t n = strlen(s);
    char *out = (char *)malloc(n + 1);
    size_t j = 0;
    int in_space = 1;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (isspace(c)) {
            if (!in_space) {
                out[j++] = ' ';
                in_space = 1;
            }
        } else {
            out[j++] = (char)c;
            in_space = 0;
        }
    }
    if (j && out[j - 1] == ' ') {
        j--;
    }
    out[j] = '\0';
    return out;
}

static int has_expected_fragment(const char *decoded, const char *expected_path, size_t match_len)
{
    FILE *fp = fopen(expected_path, "rb");
    if (!fp) {
        return 0;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *raw = (char *)malloc((size_t)size + 1U);
    if (!raw) {
        fclose(fp);
        return 0;
    }
    fread(raw, 1, (size_t)size, fp);
    raw[size] = '\0';
    fclose(fp);

    char *clean_expected = clean_text(raw);
    char *clean_decoded = clean_text(decoded);
    free(raw);

    int ok = 0;
    size_t expected_len = strlen(clean_expected);
    for (size_t i = 0; i + match_len <= expected_len; i += match_len / 2U ? match_len / 2U : 1U) {
        char saved = clean_expected[i + match_len];
        clean_expected[i + match_len] = '\0';
        if (strstr(clean_decoded, clean_expected + i)) {
            ok = 1;
            clean_expected[i + match_len] = saved;
            break;
        }
        clean_expected[i + match_len] = saved;
    }

    free(clean_expected);
    free(clean_decoded);
    return ok;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s MODE WAV EXPECTED [match_len]\n", argv[0]);
        return 2;
    }

    int16_t *samples = NULL;
    size_t count = 0;
    int rate = 0;
    if (!read_wav_mono16(argv[2], &samples, &count, &rate)) {
        fprintf(stderr, "failed to read wav: %s\n", argv[2]);
        return 1;
    }

    int16_t *codec_samples = samples;
    size_t codec_count = count;
    int codec_rate = rate;
    int16_t *resampled = NULL;
    if (rate != HOST_CODEC_RATE_HZ) {
        if (!resample_linear(samples, count, rate, HOST_CODEC_RATE_HZ,
                             &resampled, &codec_count)) {
            fprintf(stderr, "failed to resample %s from %d Hz to %d Hz\n",
                    argv[2], rate, HOST_CODEC_RATE_HZ);
            free(samples);
            return 1;
        }
        codec_samples = resampled;
        codec_rate = HOST_CODEC_RATE_HZ;
    }
    apply_input_impairments(codec_samples, codec_count);

    const int target_decode_rate = host_decode_rate_hz();
    int16_t *decode_samples = codec_samples;
    size_t decode_count = codec_count;
    int decode_rate = codec_rate;
    int16_t *decimated = NULL;
    if (codec_rate == HOST_CODEC_RATE_HZ) {
        const uint32_t decimation = (uint32_t)(HOST_CODEC_RATE_HZ / target_decode_rate);
        decimated = (int16_t *)malloc((codec_count / decimation + 1U) * sizeof(int16_t));
        if (!decimated) {
            fprintf(stderr, "out of memory decimating %s\n", argv[2]);
            free(resampled);
            free(samples);
            return 1;
        }
        audio_decim_state_t decim_state;
        audio_decim_init(&decim_state);
        decode_count = audio_decim_process(&decim_state, codec_samples, codec_count,
                                           decimated, decimation);
        decode_samples = decimated;
        decode_rate = target_decode_rate;
    }

    decode_ctx_t decoded = {0};
    const char *tone_text = getenv("PSK_RX_TONE_HZ");
    const int rx_tone_hz = tone_text ? (int)strtol(tone_text, NULL, 10) : 1500;
    const int use_modem = getenv("PSK_USE_MODEM") != NULL;
    psk_rtty_config_t cfg = {
        .mode = parse_mode(argv[1]),
        .sample_rate_hz = decode_rate,
        .rx_tone_hz = rx_tone_hz,
        .char_cb = append_char,
        .userdata = &decoded,
    };
    psk_rtty_rx_t rx;
    psk_rx_t modem;
    if (use_modem) {
        const psk_rx_config_t modem_cfg = {
            .mode = parse_app_mode(argv[1]),
            .sample_rate_hz = decode_rate,
            .rx_tone_hz = rx_tone_hz,
            .squelch = 35,
            .afc_enabled = 1,
            .squelch_enabled = 1,
        };
        psk_rx_init(&modem, &modem_cfg);
    } else {
        psk_rtty_rx_init(&rx, &cfg);
        rx.sq_thresh = 35;
    }
    const char *afc = getenv("PSK_AFC");
    if (afc) {
        if (use_modem) {
            psk_rx_set_afc(&modem, strtol(afc, NULL, 10) != 0);
        } else {
            rx.afc_enabled = strtol(afc, NULL, 10) != 0;
        }
    }
    const char *warmup = getenv("PSK_WARMUP");
    if (warmup) {
        if (use_modem) {
            modem.rx.warmup_symbols = (int)strtol(warmup, NULL, 10);
        } else {
            rx.warmup_symbols = (int)strtol(warmup, NULL, 10);
        }
    }

    const size_t block = 512;
    for (size_t pos = 0; pos < decode_count; pos += block) {
        size_t n = decode_count - pos;
        if (n > block) {
            n = block;
        }
        decoded.current_sample = pos;
        if (use_modem) {
            char frame_text[1024];
            const size_t frame_len = psk_rx_process(&modem, decode_samples + pos, n,
                                                       frame_text, sizeof(frame_text));
            for (size_t i = 0U; i < frame_len; ++i) {
                append_char(frame_text[i], &decoded);
            }
        } else {
            psk_rtty_rx_feed(&rx, decode_samples + pos, n);
        }
    }

    const psk_rtty_rx_t *active_rx = use_modem ? &modem.rx : &rx;

    size_t match_len = argc >= 5 ? (size_t)strtoul(argv[4], NULL, 10) : 60U;
    int ok = has_expected_fragment(decoded.text.data ? decoded.text.data : "", argv[3], match_len);
    const double first_ms = decoded.have_first_char ?
        ((double)decoded.first_char_sample * 1000.0 / (double)decode_rate) : -1.0;
    const float tracked_hz = (active_rx->nco_phase_inc + active_rx->freq_error) * (float)decode_rate /
                             6.28318530717958647692f;
    printf("%s decoded=%zu first_ms=%.1f symbols=%lu bits=%lu chars=%lu sq=%d/%d lock=%u "
           "tone=%d/%.1f fm=%d/%d fs=%u match=%s\n",
           argv[1], decoded.text.len, first_ms,
           (unsigned long)active_rx->debug_symbols, (unsigned long)active_rx->debug_bits,
           (unsigned long)active_rx->debug_chars, active_rx->sq_level, active_rx->sq_thresh, active_rx->lock_state,
           rx_tone_hz, tracked_hz,
           active_rx->debug_fec_metric[0], active_rx->debug_fec_metric[1], active_rx->debug_fec_selected,
           ok ? "yes" : "no");
    if (decoded.text.data) {
        fwrite(decoded.text.data, 1, decoded.text.len < 500 ? decoded.text.len : 500, stdout);
        fputc('\n', stdout);
    }

    free(decoded.text.data);
    free(decimated);
    free(resampled);
    free(samples);
    return ok ? 0 : 1;
}
