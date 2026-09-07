#include <math.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio_decim.h"
#include "audio_input.h"
#include "board_config.h"
#include "display.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "rx_modem.h"
#include "psk_tx.h"
#include "spectrum.h"
#include "transport.h"
#include "rx_web.h"
#include "ui.h"

static const char *TAG = "psk-modem";
#define TWO_PI_F 6.28318530717958647692f

typedef struct {
    char text[96];
    size_t len;
} decoded_msg_t;

typedef struct {
    int16_t samples[SPECTRUM_MAX_SAMPLES];
    size_t count;
    uint32_t sample_rate_hz;
    uint32_t source_key;
    uint32_t sequence;
} spectrum_msg_t;

typedef enum {
    TX_CMD_SET_MODE,
    TX_CMD_START_TEXT,
    TX_CMD_SET_OUTPUT_GAIN,
    TX_CMD_STOP,
} tx_cmd_type_t;

typedef struct {
    tx_cmd_type_t type;
    psk_mode_t mode;
    int tone_hz;
    int dac_volume;
    int hp_volume;
    char text[256];
} tx_cmd_t;

static QueueHandle_t s_decoded_queue;
static QueueHandle_t s_control_queue;
static QueueHandle_t s_spectrum_queue;
static QueueHandle_t s_tx_queue;
static modem_t s_audio_modem;
static int16_t s_audio_samples[256];
static char s_audio_decoded[sizeof(((decoded_msg_t *)0)->text)];
static volatile int s_display_role;
static volatile int s_tx_active;
static volatile size_t s_tx_queued;
static volatile int32_t s_input_abs_avg;
static volatile int s_waterfall_peak_hz;

#define PSK_TX_CHUNK_SAMPLES 256U
#define WATERFALL_SAMPLE_RATE_HZ SPECTRUM_SAMPLE_RATE_HZ
#define WATERFALL_SOURCE_RX 0U
#define WATERFALL_SOURCE_TX 1U
#define RX_INPUT_OPEN_ABS_AVG 500U
#define TX_TEXT_DEFAULT \
    "CQ CQ CQ DE HPM6750 PSK MODEM. THIS IS A CONTINUOUS BOARD TO BOARD AUDIO LINK TEST AT 2000 HZ. COPY THE QUICK BROWN FOX 0123456789.\n"

typedef struct {
    int16_t latest[SPECTRUM_BLOCK_SAMPLES];
    int16_t pending[SPECTRUM_BLOCK_SAMPLES];
    size_t pending_count;
    uint32_t sequence;
    audio_decim_state_t decim;
    uint32_t resample_phase;
} waterfall_stream_t;

static waterfall_stream_t s_rx_waterfall;
static waterfall_stream_t s_tx_waterfall;
static psk_tx_t s_tx_modem EXT_RAM_BSS_ATTR;
static int16_t s_tx_samples[PSK_TX_CHUNK_SAMPLES] EXT_RAM_BSS_ATTR;

static void waterfall_stream_init(waterfall_stream_t *stream);
static void append_waterfall_stream(waterfall_stream_t *stream, uint32_t source_key,
                                    const int16_t *samples, size_t count,
                                    uint32_t source_rate_hz);
static void publish_waterfall_block(uint32_t source_key, const waterfall_stream_t *stream);

static int32_t calc_block_abs_avg(const int16_t *samples, size_t count)
{
    if (count == 0) {
        return 0;
    }
    int64_t abs_sum = 0;
    for (size_t i = 0; i < count; ++i) {
        const int32_t sample = samples[i];
        abs_sum += sample < 0 ? -sample : sample;
    }
    return (int32_t)(abs_sum / (int64_t)count);
}

static void publish_decoded_text(const char *decoded, size_t decoded_len)
{
    if (decoded_len == 0) {
        return;
    }

    transport_send((const uint8_t *)decoded, decoded_len);

    decoded_msg_t msg = {0};
    msg.len = decoded_len;
    const size_t shown = decoded_len < sizeof(msg.text) - 1 ? decoded_len : sizeof(msg.text) - 1;
    for (size_t i = 0; i < shown; ++i) {
        const unsigned char ch = (unsigned char)decoded[i];
        msg.text[i] = (ch >= 0x20 && ch < 0x7f) || ch == '\r' || ch == '\n' ? (char)ch : '.';
    }
    msg.text[shown] = '\0';
    (void)xQueueSend(s_decoded_queue, &msg, 0);
}

static void apply_control_event(modem_t *modem, const ui_event_t *event)
{
    switch (event->type) {
    case UI_EVENT_RX_CONFIG:
        modem_set_psk_mode(modem, event->psk_mode);
        modem_set_rx_tone(modem, event->tone_hz);
        break;
    case UI_EVENT_MODE_CHANGED:
        modem_set_mode(modem, event->mode);
        break;
    case UI_EVENT_PSK_MODE_CHANGED:
        modem_set_psk_mode(modem, event->psk_mode);
        break;
    case UI_EVENT_TRANSPORT_CHANGED:
        transport_set_mode(event->transport_mode);
        break;
    case UI_EVENT_TONE_CHANGED:
        modem_set_rx_tone(modem, event->tone_hz);
        break;
    case UI_EVENT_INPUT_GAIN:
        (void)audio_input_set_gain_db(event->input_gain_db);
        break;
    case UI_EVENT_RX_DIAG: {
        psk_rx_debug_t rx_debug;
        psk_rx_debug_snapshot(&modem->decoder.psk_rx, &rx_debug);
        ESP_LOGI(TAG,
                 "diag psk=%s rx=%d Hz off=%d mHz peak=%d Hz abs=%d lock=%u sq=%d lvl=%d/%d stable=%d warm=%d sym=%u bits=%u z=%u o=%u dd=%u ch=%u unk=%u fec=%d/%d sel=%u locked=%u pairs=%u/%u total=%u vote=%u/%u out=%u soft=%u coarse=%u pk=%d/%d bins=%d frame=%u/%u/%u",
                 modem_psk_mode_name(modem_get_psk_mode(modem)),
                 rx_debug.tone_hz, rx_debug.carrier_offset_millihz,
                 s_waterfall_peak_hz, (int)s_input_abs_avg,
                 rx_debug.lock_state, rx_debug.sq_open, rx_debug.sq_level, rx_debug.sq_thresh,
                 rx_debug.stable_symbols, rx_debug.warmup_symbols,
                 (unsigned)rx_debug.symbols, (unsigned)rx_debug.bits,
                 (unsigned)rx_debug.zeros, (unsigned)rx_debug.ones,
                 (unsigned)rx_debug.delims, (unsigned)rx_debug.chars, (unsigned)rx_debug.unknown,
                 rx_debug.fec_metric[0], rx_debug.fec_metric[1],
                 (unsigned)rx_debug.fec_selected,
                 (unsigned)rx_debug.fec_locked_branch,
                 (unsigned)rx_debug.fec_pairs[0], (unsigned)rx_debug.fec_pairs[1],
                 (unsigned)rx_debug.fec_pair_count,
                 (unsigned)rx_debug.fec_vote_candidate, (unsigned)rx_debug.fec_vote_count,
                 (unsigned)rx_debug.fec_output_enabled,
                 (unsigned)rx_debug.soft_ambiguous,
                 (unsigned)rx_debug.coarse_updates,
                 rx_debug.timing_peak, rx_debug.timing_target, rx_debug.timing_bins,
                 (unsigned)rx_debug.valid_frames, (unsigned)rx_debug.invalid_frames,
                 (unsigned)rx_debug.frame_sync_events);
        break;
    }
    case UI_EVENT_RX_FEC_BRANCH:
        psk_rx_set_fec_branch_override(&modem->decoder.psk_rx, event->fec_branch_override);
        ESP_LOGI(TAG, "fec branch override applied: %d", event->fec_branch_override);
        break;
    case UI_EVENT_TX_MODE_CHANGED:
    case UI_EVENT_DISPLAY_ROLE_CHANGED:
    case UI_EVENT_TX_TEXT:
    case UI_EVENT_OUTPUT_GAIN:
    case UI_EVENT_NONE:
    default:
        break;
    }
}

static void send_tx_cmd(const tx_cmd_t *cmd)
{
    if (s_tx_queue && cmd) {
        (void)xQueueSend(s_tx_queue, cmd, 0);
    }
}

static void waterfall_stream_init(waterfall_stream_t *stream)
{
    if (!stream) {
        return;
    }

    memset(stream, 0, sizeof(*stream));
    audio_decim_init(&stream->decim);
}

static void publish_waterfall_block(uint32_t source_key, const waterfall_stream_t *stream)
{
    if (!s_spectrum_queue || !stream) {
        return;
    }
    if ((source_key == WATERFALL_SOURCE_TX) != (s_display_role != 0)) {
        return;
    }

    spectrum_msg_t msg = {
        .count = SPECTRUM_BLOCK_SAMPLES,
        .sample_rate_hz = WATERFALL_SAMPLE_RATE_HZ,
        .source_key = source_key,
        .sequence = stream->sequence,
    };
    memcpy(msg.samples, stream->latest, sizeof(msg.samples));
    (void)xQueueOverwrite(s_spectrum_queue, &msg);
}

static void waterfall_stream_append_samples(waterfall_stream_t *stream, uint32_t source_key,
                                            const int16_t *samples, size_t count)
{
    if (!stream || !samples || count == 0U) {
        return;
    }

    size_t offset = 0U;
    while (offset < count) {
        const size_t room = SPECTRUM_BLOCK_SAMPLES - stream->pending_count;
        size_t chunk = count - offset;
        if (chunk > room) {
            chunk = room;
        }
        memcpy(&stream->pending[stream->pending_count], &samples[offset],
               chunk * sizeof(stream->pending[0]));
        stream->pending_count += chunk;
        offset += chunk;

        if (stream->pending_count >= SPECTRUM_BLOCK_SAMPLES) {
            memcpy(stream->latest, stream->pending, sizeof(stream->latest));
            stream->pending_count = 0U;
            ++stream->sequence;
            publish_waterfall_block(source_key, stream);
        }
    }
}

static void append_waterfall_stream(waterfall_stream_t *stream, uint32_t source_key,
                                    const int16_t *samples, size_t count,
                                    uint32_t source_rate_hz)
{
    if (!stream || !samples || count == 0U || source_rate_hz == 0U) {
        return;
    }

    if (source_rate_hz == WATERFALL_SAMPLE_RATE_HZ || source_rate_hz < WATERFALL_SAMPLE_RATE_HZ) {
        waterfall_stream_append_samples(stream, source_key, samples, count);
        return;
    }

    if ((source_rate_hz % WATERFALL_SAMPLE_RATE_HZ) == 0U) {
        int16_t preview[PSK_TX_CHUNK_SAMPLES];
        const uint32_t decimation = source_rate_hz / WATERFALL_SAMPLE_RATE_HZ;
        size_t offset = 0U;

        while (offset < count) {
            size_t chunk = count - offset;
            if (chunk > PSK_TX_CHUNK_SAMPLES) {
                chunk = PSK_TX_CHUNK_SAMPLES;
            }
            const size_t preview_count = audio_decim_process(&stream->decim, &samples[offset],
                                                             chunk, preview, decimation);
            waterfall_stream_append_samples(stream, source_key, preview, preview_count);
            offset += chunk;
        }
        return;
    }

    int16_t preview[PSK_TX_CHUNK_SAMPLES];
    size_t offset = 0U;

    while (offset < count) {
        size_t preview_count = 0U;
        while (offset < count && preview_count < PSK_TX_CHUNK_SAMPLES) {
            stream->resample_phase += WATERFALL_SAMPLE_RATE_HZ;
            if (stream->resample_phase >= source_rate_hz) {
                stream->resample_phase -= source_rate_hz;
                preview[preview_count++] = samples[offset];
            }
            ++offset;
        }
        waterfall_stream_append_samples(stream, source_key, preview, preview_count);
    }
}

static void clear_display_spectrum(spectrum_t *spectrum)
{
    if (!spectrum) {
        return;
    }
    memset(spectrum, 0, sizeof(*spectrum));
    spectrum->peak_dbfs = -120.0f;
    spectrum_reset();
}

static void tx_task(void *arg)
{
    (void)arg;

    psk_tx_t *tx = &s_tx_modem;
    psk_tx_init(tx, BOARD_CODEC_SAMPLE_RATE_HZ);
    psk_mode_t tx_mode = PSK_MODE_BPSK31;
    int tx_tone_hz = 2000;
    char tx_text[sizeof(((tx_cmd_t *)0)->text)] = TX_TEXT_DEFAULT;
    uint8_t tx_loop_enabled = 0U;

    ESP_LOGI(TAG, "tx task started");

    while (true) {
        tx_cmd_t cmd;
        while (xQueueReceive(s_tx_queue, &cmd, 0) == pdTRUE) {
            switch (cmd.type) {
            case TX_CMD_SET_MODE:
                tx_mode = cmd.mode;
                if (tx_loop_enabled) {
                    psk_tx_stop(tx);
                    audio_output_clear();
                    waterfall_stream_init(&s_tx_waterfall);
                    if (psk_tx_start_text_loop(tx, tx_mode, tx_tone_hz, tx_text) != 0) {
                        tx_loop_enabled = 0U;
                    }
                }
                break;
            case TX_CMD_SET_OUTPUT_GAIN:
                (void)audio_output_set_codec_gain((uint8_t)cmd.dac_volume, (uint8_t)cmd.hp_volume);
                break;
            case TX_CMD_START_TEXT:
                tx_mode = cmd.mode;
                tx_tone_hz = cmd.tone_hz > 0 ? cmd.tone_hz : 2000;
                if (cmd.text[0] != '\0') {
                    strncpy(tx_text, cmd.text, sizeof(tx_text) - 1U);
                    tx_text[sizeof(tx_text) - 1U] = '\0';
                }
                audio_output_clear();
                waterfall_stream_init(&s_tx_waterfall);
                if (psk_tx_start_text_loop(tx, tx_mode, tx_tone_hz, tx_text) == 0) {
                    tx_loop_enabled = 1U;
                    s_tx_active = 1;
                    ESP_LOGI(TAG, "tx start mode=%s tone=%d text='%s'",
                             psk_mode_name(tx_mode), tx_tone_hz, tx_text);
                } else {
                    tx_loop_enabled = 0U;
                    s_tx_active = 0;
                    ESP_LOGW(TAG, "tx start failed mode=%s text_len=%u",
                             psk_mode_name(tx_mode), (unsigned)strlen(tx_text));
                }
                break;
            case TX_CMD_STOP:
                tx_loop_enabled = 0U;
                psk_tx_stop(tx);
                audio_output_clear();
                waterfall_stream_init(&s_tx_waterfall);
                s_tx_active = 0;
                ESP_LOGI(TAG, "tx stop");
                break;
            default:
                break;
            }
        }

        if (!psk_tx_active(tx) && tx_loop_enabled) {
            if (psk_tx_start_text_loop(tx, tx_mode, tx_tone_hz, tx_text) != 0) {
                tx_loop_enabled = 0U;
            }
        }

        if (psk_tx_active(tx)) {
            const size_t generated = psk_tx_generate(tx, s_tx_samples,
                                                     sizeof(s_tx_samples) / sizeof(s_tx_samples[0]));
            if (generated > 0) {
                const size_t written = audio_output_write(s_tx_samples, generated);
                s_tx_queued = audio_output_queued();
                append_waterfall_stream(&s_tx_waterfall, WATERFALL_SOURCE_TX, s_tx_samples,
                                        generated, BOARD_CODEC_SAMPLE_RATE_HZ);
                if (written == 0) {
                    vTaskDelay(pdMS_TO_TICKS(2));
                }
            }
        } else {
            s_tx_queued = audio_output_queued();
            vTaskDelay(pdMS_TO_TICKS(2));
        }

        s_tx_active = (tx_loop_enabled || psk_tx_active(tx) || audio_output_queued() > 0U) ? 1 : 0;

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static bool web_configure_receiver(psk_mode_t mode, int tone_hz)
{
    const ui_event_t event = {.type = UI_EVENT_RX_CONFIG, .psk_mode = mode, .tone_hz = tone_hz};
    return s_control_queue && xQueueSend(s_control_queue, &event, 0) == pdTRUE;
}

static void audio_task(void *arg)
{
    (void)arg;
    const modem_config_t config = {
        .mode = MODEM_MODE_BPSK31, .psk_mode = PSK_MODE_BPSK31,
        .sample_rate_hz = BOARD_MODEM_SAMPLE_RATE_HZ, .rx_tone_hz = 2000, .squelch = 35,
    };
    modem_t *modem = &s_audio_modem;
    modem_init(modem, &config);
    rx_web_status_t status = {0};
    uint64_t dsp_sum = 0, dsp_samples = 0;
    uint32_t samples = 0, dsp_max = 0;
    uint8_t input_open = 0, closed_blocks = 0;
    int64_t window_start = esp_timer_get_time(), last_yield = window_start;
    ESP_LOGI(TAG, "audio decode task started: %u samples/block", (unsigned)(sizeof(s_audio_samples)/sizeof(s_audio_samples[0])));
    for (;;) {
        ui_event_t event;
        /* Bound control work so clients cannot starve I2S by flooding commands. */
        for (unsigned i = 0; i < 4 && xQueueReceive(s_control_queue, &event, 0) == pdTRUE; ++i) {
            apply_control_event(modem, &event);
            status.mode = modem_get_psk_mode(modem);
            status.tone_hz = modem->config.rx_tone_hz;
            rx_web_set_status(&status);
        }
        const size_t count = audio_input_read(s_audio_samples,
            sizeof(s_audio_samples)/sizeof(s_audio_samples[0]), 50);
        if (count) {
            samples += count;
            append_waterfall_stream(&s_rx_waterfall, WATERFALL_SOURCE_RX,
                                    s_audio_samples, count, BOARD_MODEM_SAMPLE_RATE_HZ);
            s_input_abs_avg = calc_block_abs_avg(s_audio_samples, count);
            if (s_input_abs_avg >= RX_INPUT_OPEN_ABS_AVG) {
                if (!input_open) {
                    modem_set_psk_mode(modem, modem_get_psk_mode(modem));
                    input_open = 1;
                }
                closed_blocks = 0;
            } else if (closed_blocks < 10) { /* about 53 ms at 256 / 48 kHz */
                ++closed_blocks;
            } else {
                input_open = 0;
            }
            status.block_budget_us = (uint32_t)((uint64_t)count * 1000000 / BOARD_MODEM_SAMPLE_RATE_HZ);
            if (input_open) {
                const int64_t start = esp_timer_get_time();
                const size_t len = modem_process(modem, s_audio_samples, count,
                                                 s_audio_decoded, sizeof(s_audio_decoded));
                const uint32_t elapsed = (uint32_t)(esp_timer_get_time() - start);
                dsp_sum += elapsed;
                dsp_samples += count;
                if (elapsed > dsp_max) dsp_max = elapsed;
                if (elapsed > status.block_budget_us) ++status.deadline_misses;
                publish_decoded_text(s_audio_decoded, len);
            }
        } else {
            ++status.read_errors;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        const int64_t now = esp_timer_get_time();
        const uint64_t window = (uint64_t)(now - window_start);
        if (window >= 1000000) {
            psk_rx_debug_t debug;
            psk_rx_debug_snapshot(&modem->decoder.psk_rx, &debug);
            status.mode = modem_get_psk_mode(modem);
            status.tone_hz = modem->config.rx_tone_hz;
            status.lock_state = input_open ? debug.lock_state : 0;
            status.input_level = (unsigned)s_input_abs_avg;
            status.samples_per_second = (unsigned)((uint64_t)samples * 1000000 / window);
            status.dsp_load_permille = dsp_samples ? (unsigned)(dsp_sum * BOARD_MODEM_SAMPLE_RATE_HZ / (dsp_samples * 1000)) : 0;
            status.dsp_max_us = dsp_max;
            rx_web_set_status(&status);
#if BOARD_MODEM_DEBUG_LOGS
            ESP_LOGI(TAG, "RX %s load=%u.%u%% max=%uus budget=%uus late=%" PRIu32 " samples/s=%u",
                     psk_mode_name(status.mode), status.dsp_load_permille/10,
                     status.dsp_load_permille%10, dsp_max, status.block_budget_us,
                     status.deadline_misses, status.samples_per_second);
#endif
            samples = 0; dsp_sum = 0; dsp_samples = 0; dsp_max = 0;
            window_start = now;
        }
        /* I2S normally blocks. Still let core 0 idle run under sustained overload. */
        if (now - last_yield >= 20000) {
            vTaskDelay(pdMS_TO_TICKS(1));
            last_yield = esp_timer_get_time();
        }
    }
}

static void ui_task(void *arg)
{
    static display_state_t display_state EXT_RAM_BSS_ATTR;
    static spectrum_t spectrum EXT_RAM_BSS_ATTR;
    static spectrum_msg_t spectrum_msg EXT_RAM_BSS_ATTR;

    (void)arg;

    memset(&display_state, 0, sizeof(display_state));
    display_state.selected_mode = MODEM_MODE_BPSK31;
    display_state.selected_psk_mode = PSK_MODE_BPSK31;
    display_state.selected_tx_psk_mode = PSK_MODE_BPSK31;
    display_state.transport_mode = TRANSPORT_MODE_USB_SERIAL;
    display_state.rx_tone_hz = 2000;
    display_state.tx_tone_hz = 2000;
    strncpy(display_state.tx_text, TX_TEXT_DEFAULT, sizeof(display_state.tx_text) - 1U);
    display_state.tx_text[sizeof(display_state.tx_text) - 1U] = '\0';

    uint32_t last_waterfall_sequence = 0U;
    uint32_t last_waterfall_source_key = 0xffffffffUL;
    uint32_t last_waterfall_sample_rate_hz = 0U;
    int64_t last_render_us = 0;
    psk_mode_t last_rx_mode = PSK_MODE_BPSK31;
    int last_rx_tone = 2000;

    clear_display_spectrum(&spectrum);
    ESP_LOGI(TAG, "ui task started");

    while (true) {
        decoded_msg_t msg;
        while (xQueueReceive(s_decoded_queue, &msg, 0) == pdTRUE) {
            display_append_text(&display_state, msg.text);
        }

        rx_web_status_t actual;
        rx_web_get_status(&actual);
        if (actual.mode != last_rx_mode || actual.tone_hz != last_rx_tone) {
            last_rx_mode = actual.mode; last_rx_tone = actual.tone_hz;
            display_state.selected_psk_mode = actual.mode;
            display_state.selected_mode = actual.mode == PSK_MODE_BPSK31 ? MODEM_MODE_BPSK31 : MODEM_MODE_PSK63;
            display_state.rx_tone_hz = actual.tone_hz;
        }
        display_state.transport_mode = transport_get_mode();
        const ui_event_t event = ui_poll(&display_state);
        switch (event.type) {
        case UI_EVENT_MODE_CHANGED:
            display_state.selected_mode = event.mode;
            if (event.mode == MODEM_MODE_BPSK31) {
                display_state.selected_psk_mode = PSK_MODE_BPSK31;
            } else if (event.mode == MODEM_MODE_PSK63 && display_state.selected_psk_mode == PSK_MODE_BPSK31) {
                display_state.selected_psk_mode = PSK_MODE_BPSK63;
            }
            if (display_state.rx_tone_hz < modem_mode_band_min_hz(event.mode) ||
                display_state.rx_tone_hz > modem_mode_band_max_hz(event.mode)) {
                display_state.rx_tone_hz = (modem_mode_band_min_hz(event.mode) +
                                           modem_mode_band_max_hz(event.mode)) / 2;
                ui_event_t tone_event = {
                    .type = UI_EVENT_TONE_CHANGED,
                    .tone_hz = display_state.rx_tone_hz,
                };
                (void)xQueueSend(s_control_queue, &tone_event, 0);
            }
            (void)xQueueSend(s_control_queue, &event, 0);
            clear_display_spectrum(&spectrum);
            last_waterfall_sequence = 0U;
            last_waterfall_source_key = 0xffffffffUL;
            last_waterfall_sample_rate_hz = 0U;
            break;
        case UI_EVENT_PSK_MODE_CHANGED:
            display_state.selected_psk_mode = event.psk_mode;
            display_state.selected_mode = event.psk_mode == PSK_MODE_BPSK31 ? MODEM_MODE_BPSK31 : MODEM_MODE_PSK63;
            (void)xQueueSend(s_control_queue, &event, 0);
            clear_display_spectrum(&spectrum);
            last_waterfall_sequence = 0U;
            last_waterfall_source_key = 0xffffffffUL;
            last_waterfall_sample_rate_hz = 0U;
            break;
        case UI_EVENT_TRANSPORT_CHANGED:
            display_state.transport_mode = event.transport_mode;
            (void)xQueueSend(s_control_queue, &event, 0);
            break;
        case UI_EVENT_TONE_CHANGED:
            display_state.rx_tone_hz = event.tone_hz;
            (void)xQueueSend(s_control_queue, &event, 0);
            clear_display_spectrum(&spectrum);
            last_waterfall_sequence = 0U;
            last_waterfall_source_key = 0xffffffffUL;
            last_waterfall_sample_rate_hz = 0U;
            break;
        case UI_EVENT_TX_MODE_CHANGED: {
            display_state.selected_tx_psk_mode = event.tx_psk_mode;
            tx_cmd_t cmd = {
                .type = TX_CMD_SET_MODE,
                .mode = event.tx_psk_mode,
            };
            send_tx_cmd(&cmd);
            clear_display_spectrum(&spectrum);
            last_waterfall_sequence = 0U;
            last_waterfall_source_key = 0xffffffffUL;
            last_waterfall_sample_rate_hz = 0U;
            break;
        }
        case UI_EVENT_DISPLAY_ROLE_CHANGED:
            display_state.display_role = event.display_role != 0;
            s_display_role = display_state.display_role;
            if (display_state.display_role) {
                tx_cmd_t cmd = {
                    .type = TX_CMD_START_TEXT,
                    .mode = display_state.selected_tx_psk_mode,
                    .tone_hz = display_state.tx_tone_hz,
                };
                size_t text_len = strnlen(display_state.tx_text, sizeof(cmd.text) - 1);
                memcpy(cmd.text, display_state.tx_text, text_len);
                cmd.text[text_len] = '\0';
                send_tx_cmd(&cmd);
            } else {
                tx_cmd_t cmd = {
                    .type = TX_CMD_STOP,
                };
                send_tx_cmd(&cmd);
            }
            clear_display_spectrum(&spectrum);
            last_waterfall_sequence = 0U;
            last_waterfall_source_key = 0xffffffffUL;
            last_waterfall_sample_rate_hz = 0U;
            break;
        case UI_EVENT_TX_TEXT: {
            display_state.display_role = 1;
            s_display_role = 1;
            display_state.selected_tx_psk_mode = event.tx_psk_mode;
            display_state.tx_tone_hz = event.tx_tone_hz > 0 ? event.tx_tone_hz : 2000;
            strncpy(display_state.tx_text, event.tx_text, sizeof(display_state.tx_text) - 1);
            display_state.tx_text[sizeof(display_state.tx_text) - 1] = '\0';
            tx_cmd_t cmd = {
                .type = TX_CMD_START_TEXT,
                .mode = event.tx_psk_mode,
                .tone_hz = display_state.tx_tone_hz,
            };
            size_t text_len = strnlen(display_state.tx_text, sizeof(cmd.text) - 1);
            memcpy(cmd.text, display_state.tx_text, text_len);
            cmd.text[text_len] = '\0';
            send_tx_cmd(&cmd);
            clear_display_spectrum(&spectrum);
            last_waterfall_sequence = 0U;
            last_waterfall_source_key = 0xffffffffUL;
            last_waterfall_sample_rate_hz = 0U;
            break;
        }
        case UI_EVENT_OUTPUT_GAIN: {
            tx_cmd_t cmd = {
                .type = TX_CMD_SET_OUTPUT_GAIN,
                .dac_volume = event.output_dac_volume,
                .hp_volume = event.output_hp_volume,
            };
            send_tx_cmd(&cmd);
            break;
        }
        case UI_EVENT_INPUT_GAIN:
            (void)audio_input_set_gain_db(event.input_gain_db);
            break;
        case UI_EVENT_RX_DIAG:
            (void)xQueueSend(s_control_queue, &event, 0);
            break;
        case UI_EVENT_RX_FEC_BRANCH:
            (void)xQueueSend(s_control_queue, &event, 0);
            break;
        case UI_EVENT_TX_STOP: {
            tx_cmd_t cmd = {
                .type = TX_CMD_STOP,
            };
            send_tx_cmd(&cmd);
            display_state.tx_active = 0;
            display_state.tx_queued = 0;
            clear_display_spectrum(&spectrum);
            last_waterfall_sequence = 0U;
            last_waterfall_source_key = 0xffffffffUL;
            last_waterfall_sample_rate_hz = 0U;
            break;
        }
        case UI_EVENT_NONE:
        default:
            break;
        }

        const int64_t now_us = esp_timer_get_time();
        if (now_us - last_render_us >= 100000) {
            last_render_us = now_us;
            display_state.tx_active = s_tx_active;
            display_state.tx_queued = s_tx_queued;
            display_state.input_abs_avg = s_input_abs_avg > 65535 ? 65535U : (uint16_t)s_input_abs_avg;
            display_state.selected_lane = BOARD_AUDIO_INPUT_CHANNEL == 1 ? 1U : 0U;
            display_state.update_waterfall = 0;
            if (xQueueReceive(s_spectrum_queue, &spectrum_msg, 0) == pdTRUE) {
                if (spectrum_msg.source_key != last_waterfall_source_key ||
                    spectrum_msg.sample_rate_hz != last_waterfall_sample_rate_hz) {
                    clear_display_spectrum(&spectrum);
                    last_waterfall_sequence = 0U;
                    last_waterfall_source_key = spectrum_msg.source_key;
                    last_waterfall_sample_rate_hz = spectrum_msg.sample_rate_hz;
                }
                if (spectrum_msg.sequence != last_waterfall_sequence) {
                    spectrum_analyze(spectrum_msg.samples, spectrum_msg.count,
                                     (int)spectrum_msg.sample_rate_hz, &spectrum);
                    last_waterfall_sequence = spectrum_msg.sequence;
                    s_waterfall_peak_hz = spectrum.peak_hz;
                    display_state.update_waterfall = 1;
                }
            }
            display_render(&display_state, &spectrum);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(ui_init());
    ESP_ERROR_CHECK(transport_init(TRANSPORT_MODE_USB_SERIAL));
    ESP_ERROR_CHECK(audio_input_init());
    ESP_ERROR_CHECK(audio_input_set_gain_db(BOARD_AUDIO_INPUT_GAIN_DB));

    s_decoded_queue = xQueueCreate(8, sizeof(decoded_msg_t));
    s_control_queue = xQueueCreate(12, sizeof(ui_event_t));
    s_spectrum_queue = xQueueCreate(1, sizeof(spectrum_msg_t));
    s_tx_queue = xQueueCreate(4, sizeof(tx_cmd_t));
    ESP_ERROR_CHECK(s_decoded_queue && s_control_queue && s_spectrum_queue && s_tx_queue ?
                    ESP_OK : ESP_ERR_NO_MEM);

    BaseType_t task_ok = xTaskCreatePinnedToCore(audio_task, "psk_audio", 16384, NULL, 8, NULL, 0);
    ESP_ERROR_CHECK(task_ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    task_ok = xTaskCreatePinnedToCore(tx_task, "psk_tx", 8192, NULL, 7, NULL, 0);
    ESP_ERROR_CHECK(task_ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    task_ok = xTaskCreatePinnedToCore(ui_task, "psk_ui", 8192, NULL, 2, NULL, 1);
    ESP_ERROR_CHECK(task_ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_ERROR_CHECK(rx_web_start(web_configure_receiver));
    ESP_LOGI(TAG, "ESP32-P4 PSK modem started");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
