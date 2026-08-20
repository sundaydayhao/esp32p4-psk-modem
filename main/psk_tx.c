#include "psk_tx.h"

#include <math.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_input.h"
#include "board_config.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "ft8/constants.h"
#include "ft8/encode.h"
#include "ft8/message.h"
#include "rx_modem.h"
#include "psk_modes.h"
#include "PSKTables.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TWO_PI_F 6.28318530717958647692f
#define TX_QUEUE_DEPTH 4
#define TX_WAVE_CHUNK 512
#define TX_AMPLITUDE 11000

static const char *TAG = "tx";
static QueueHandle_t s_tx_queue;

static const uint8_t s_baudot_letters[26] = {
    0x03, 0x19, 0x0e, 0x09, 0x01, 0x0d, 0x1a, 0x14, 0x06, 0x0b, 0x0f, 0x12, 0x1c,
    0x0c, 0x18, 0x16, 0x17, 0x0a, 0x05, 0x10, 0x07, 0x1e, 0x13, 0x1b, 0x15, 0x11,
};

static const uint8_t s_baudot_figures[26] = {
    0x03, 0x19, 0x0e, 0x09, 0x01, 0x0d, 0x1a, 0x14, 0x06, 0x0b, 0x0f, 0x12, 0x1c,
    0x0c, 0x18, 0x16, 0x17, 0x0a, 0x05, 0x10, 0x07, 0x1e, 0x13, 0x1b, 0x15, 0x11,
};

typedef struct {
    const tx_request_t *request;
    int16_t buffer[TX_WAVE_CHUNK];
    size_t buffer_len;
    float phase;
} tx_synth_t;

static void tx_flush(tx_synth_t *s)
{
    if (s->buffer_len == 0) {
        return;
    }
    (void)audio_output_write(s->buffer, s->buffer_len);
    s->buffer_len = 0;
}

static void tx_push_sample(tx_synth_t *s, float sample)
{
    if (sample > 32767.0f) {
        sample = 32767.0f;
    } else if (sample < -32768.0f) {
        sample = -32768.0f;
    }
    s->buffer[s->buffer_len++] = (int16_t)lrintf(sample);
    if (s->buffer_len == TX_WAVE_CHUNK) {
        tx_flush(s);
    }
}

static void tx_emit_silence(tx_synth_t *s, float seconds)
{
    const int samples = (int)lrintf(seconds * (float)BOARD_CODEC_SAMPLE_RATE_HZ);
    for (int i = 0; i < samples; ++i) {
        tx_push_sample(s, 0.0f);
    }
}

static void tx_emit_tone(tx_synth_t *s, float freq_hz, float seconds, float phase_offset, float amplitude)
{
    const int samples = (int)lrintf(seconds * (float)BOARD_CODEC_SAMPLE_RATE_HZ);
    const float phase_inc = TWO_PI_F * freq_hz / (float)BOARD_CODEC_SAMPLE_RATE_HZ;
    float phase = s->phase + phase_offset;
    for (int i = 0; i < samples; ++i) {
        tx_push_sample(s, sinf(phase) * amplitude);
        phase += phase_inc;
        if (phase > TWO_PI_F) {
            phase -= TWO_PI_F;
        }
    }
    s->phase = phase;
}

static size_t varicode_bits(uint8_t ch, uint8_t *bits, size_t max_bits)
{
    if (max_bits < 2) {
        return 0;
    }
    const unsigned int code = VARICODE_TABLE[ch];
    if (code == 0) {
        bits[0] = 0;
        bits[1] = 0;
        return 2;
    }

    const int msb = 31 - __builtin_clz(code);
    size_t count = 0;
    for (int bit = msb; bit >= 0 && count < max_bits; --bit) {
        bits[count++] = (code >> bit) & 1U;
    }
    if (count + 2 <= max_bits) {
        bits[count++] = 0;
        bits[count++] = 0;
    }
    return count;
}

static size_t build_varicode_stream(const char *text, uint8_t *bits, size_t max_bits)
{
    size_t count = 0;
    if (!text) {
        return 0;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p && count + 8 < max_bits; ++p) {
        count += varicode_bits(*p, &bits[count], max_bits - count);
    }
    return count;
}

static size_t bits_to_symbols(const uint8_t *bits, size_t bit_count, uint8_t bits_per_symbol,
                              uint8_t *symbols, size_t max_symbols)
{
    size_t out = 0;
    size_t pos = 0;
    while (pos < bit_count && out < max_symbols) {
        uint8_t sym = 0;
        for (uint8_t i = 0; i < bits_per_symbol; ++i) {
            sym <<= 1;
            if (pos < bit_count) {
                sym |= bits[pos++];
            }
        }
        symbols[out++] = sym;
    }
    return out;
}

static void emit_psk_symbols(tx_synth_t *s, const psk_mode_info_t *info, const uint8_t *symbols,
                             size_t symbol_count)
{
    const float baud = info->baud > 0.0f ? info->baud : 31.25f;
    const float symbol_seconds = 1.0f / baud;
    const float carrier_spacing = baud;
    const int carriers = info->carriers > 0 ? info->carriers : 1;
    const float base_freq = (float)1000 + (float)(s->request->tone_hz - 1000);

    for (size_t i = 0; i < symbol_count; ++i) {
        const uint8_t sym = symbols[i];
        float phase_step = 0.0f;
        if (info->bits_per_symbol == 1) {
            phase_step = sym ? 0.0f : (float)M_PI;
        } else {
            const uint8_t phase_levels = 1U << info->bits_per_symbol;
            phase_step = TWO_PI_F * (float)(sym % phase_levels) / (float)phase_levels;
        }

        const int samples = (int)lrintf(symbol_seconds * (float)BOARD_CODEC_SAMPLE_RATE_HZ);
        for (int n = 0; n < samples; ++n) {
            float mix = 0.0f;
            for (int c = 0; c < carriers; ++c) {
                const float offset = ((float)c - ((float)carriers - 1.0f) * 0.5f) * carrier_spacing;
                const float phase = s->phase + phase_step + TWO_PI_F * (base_freq + offset) * (float)n /
                                                     (float)BOARD_CODEC_SAMPLE_RATE_HZ;
                mix += sinf(phase);
            }
            tx_push_sample(s, (mix / (float)carriers) * TX_AMPLITUDE);
        }
        s->phase += phase_step;
        while (s->phase > TWO_PI_F) {
            s->phase -= TWO_PI_F;
        }
    }
}

static void tx_send_psk(tx_synth_t *s)
{
    const psk_mode_info_t *info = psk_mode_info(s->request->psk_mode);
    uint8_t bits[2048];
    uint8_t symbols[1024];
    const size_t bit_count = build_varicode_stream(s->request->text, bits, sizeof(bits));
    const size_t symbol_count = bits_to_symbols(bits, bit_count, info->bits_per_symbol, symbols, sizeof(symbols));
    tx_emit_tone(s, (float)s->request->tone_hz, 0.15f, 0.0f, TX_AMPLITUDE * 0.5f);
    emit_psk_symbols(s, info, symbols, symbol_count);
    tx_emit_tone(s, (float)s->request->tone_hz, 0.05f, 0.0f, TX_AMPLITUDE * 0.5f);
}

static void tx_send_cw(tx_synth_t *s)
{
    static const struct {
        char ch;
        const char *morse;
    } table[] = {
        {'A', ".-"}, {'B', "-..."}, {'C', "-.-."}, {'D', "-.."}, {'E', "."},
        {'F', "..-."}, {'G', "--."}, {'H', "...."}, {'I', ".."}, {'J', ".---"},
        {'K', "-.-"}, {'L', ".-.."}, {'M', "--"}, {'N', "-."}, {'O', "---"},
        {'P', ".--."}, {'Q', "--.-"}, {'R', ".-."}, {'S', "..."}, {'T', "-"},
        {'U', "..-"}, {'V', "...-"}, {'W', ".--"}, {'X', "-..-"}, {'Y', "-.--"},
        {'Z', "--.."}, {'1', ".----"}, {'2', "..---"}, {'3', "...--"},
        {'4', "....-"}, {'5', "....."}, {'6', "-...."}, {'7', "--..."},
        {'8', "---.."}, {'9', "----."}, {'0', "-----"},
    };
    const float wpm = 18.0f;
    const float unit = 1.2f / wpm;
    for (const char *p = s->request->text; *p; ++p) {
        if (*p == ' ') {
            tx_emit_silence(s, unit * 7.0f);
            continue;
        }
        char up = (char)toupper((unsigned char)*p);
        const char *morse = NULL;
        for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
            if (table[i].ch == up) {
                morse = table[i].morse;
                break;
            }
        }
        if (!morse) {
            continue;
        }
        for (const char *m = morse; *m; ++m) {
            tx_emit_tone(s, (float)s->request->tone_hz, *m == '.' ? unit : unit * 3.0f, 0.0f, TX_AMPLITUDE * 0.5f);
            tx_emit_silence(s, unit);
        }
        tx_emit_silence(s, unit * 2.0f);
    }
}

static uint8_t rtty_code_for_char(char ch, int *figures)
{
    if (ch == ' ') {
        return 0x04;
    }
    if (ch >= 'A' && ch <= 'Z') {
        *figures = 0;
        return s_baudot_letters[ch - 'A'];
    }
    if (ch >= 'a' && ch <= 'z') {
        *figures = 0;
        return s_baudot_letters[ch - 'a'];
    }
    if (ch >= '0' && ch <= '9') {
        *figures = 1;
        return s_baudot_figures[ch - '0'];
    }
    *figures = 0;
    return 0x04;
}

static void tx_send_rtty(tx_synth_t *s)
{
    const float baud = 45.45f;
    const float bit_seconds = 1.0f / baud;
    int figures = 0;
    for (const char *p = s->request->text; *p; ++p) {
        const uint8_t code = rtty_code_for_char(*p, &figures);
        tx_emit_silence(s, bit_seconds);
        tx_emit_tone(s, (float)(s->request->tone_hz - 85), bit_seconds, 0.0f, TX_AMPLITUDE * 0.45f);
        for (int bit = 0; bit < 5; ++bit) {
            const int v = (code >> bit) & 1;
            tx_emit_tone(s, (float)(s->request->tone_hz + (v ? 85 : -85)), bit_seconds, 0.0f, TX_AMPLITUDE * 0.45f);
        }
        tx_emit_tone(s, (float)(s->request->tone_hz + 85), bit_seconds * 2.0f, 0.0f, TX_AMPLITUDE * 0.45f);
    }
}

static void tx_send_ftx(tx_synth_t *s, int ft4)
{
    ftx_message_t msg;
    memset(&msg, 0, sizeof(msg));
    static const ftx_callsign_hash_interface_t hash_if = { 0 };
    if (ftx_message_encode(&msg, (ftx_callsign_hash_interface_t *)&hash_if, s->request->text) != FTX_MESSAGE_RC_OK) {
        (void)ftx_message_encode_free(&msg, s->request->text);
    }

    uint8_t tones[FT8_NN > FT4_NN ? FT8_NN : FT4_NN];
    if (ft4) {
        ft4_encode(msg.payload, tones);
    } else {
        ft8_encode(msg.payload, tones);
    }

    const float sym_seconds = ft4 ? 0.06f : 0.16f;
    const float tone_step = ft4 ? 6.25f : 6.25f;
    const float base = (float)s->request->tone_hz;
    tx_emit_silence(s, 0.5f);
    for (int i = 0; i < (ft4 ? FT4_NN : FT8_NN); ++i) {
        const float freq = base + (float)tones[i] * tone_step;
        tx_emit_tone(s, freq, sym_seconds, 0.0f, TX_AMPLITUDE * 0.42f);
    }
    tx_emit_silence(s, 0.25f);
}

static void tx_send_mfsk16(tx_synth_t *s)
{
    uint8_t bits[2048];
    uint8_t symbols[1024];
    const size_t bit_count = build_varicode_stream(s->request->text, bits, sizeof(bits));
    const size_t symbol_count = bits_to_symbols(bits, bit_count, 4, symbols, sizeof(symbols));
    tx_emit_silence(s, 0.15f);
    for (size_t i = 0; i < symbol_count; ++i) {
        const float freq = (float)s->request->tone_hz + ((float)(symbols[i] & 0x0fU) - 7.5f) * 20.0f;
        tx_emit_tone(s, freq, 1.0f / 15.625f, 0.0f, TX_AMPLITUDE * 0.42f);
    }
    tx_emit_silence(s, 0.05f);
}

static void tx_send_request(const tx_request_t *request)
{
    tx_synth_t s = {
        .request = request,
        .buffer_len = 0,
        .phase = 0.0f,
    };
    const psk_mode_info_t *info = psk_mode_info(request->psk_mode);

    ESP_LOGI(TAG, "tx start mode=%s psk=%s text=%s", modem_mode_name(request->mode),
             info->name, request->text);

    switch (request->mode) {
    case MODEM_MODE_BPSK31:
    case MODEM_MODE_PSK63:
        tx_send_psk(&s);
        break;
    case MODEM_MODE_FT8:
        tx_send_ftx(&s, 0);
        break;
    case MODEM_MODE_FT4:
        tx_send_ftx(&s, 1);
        break;
    case MODEM_MODE_RTTY:
        tx_send_rtty(&s);
        break;
    case MODEM_MODE_CW:
        tx_send_cw(&s);
        break;
    case MODEM_MODE_MFSK16:
        tx_send_mfsk16(&s);
        break;
    default:
        tx_send_psk(&s);
        break;
    }
    tx_flush(&s);
}

esp_err_t tx_init(void)
{
    s_tx_queue = xQueueCreate(TX_QUEUE_DEPTH, sizeof(tx_request_t));
    return s_tx_queue ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t tx_submit(const tx_request_t *request)
{
    if (!request || !s_tx_queue) {
        return ESP_ERR_INVALID_ARG;
    }
    return xQueueSend(s_tx_queue, request, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

void tx_task(void *arg)
{
    (void)arg;
    tx_request_t request;
    ESP_LOGI(TAG, "tx task started");
    while (true) {
        if (xQueueReceive(s_tx_queue, &request, portMAX_DELAY) == pdTRUE) {
            tx_send_request(&request);
        }
    }
}
