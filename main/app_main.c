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
#include "esp_heap_caps.h"
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

typedef struct {
    psk_mode_t mode;
    int tone_hz, dac_volume, hp_volume;
    content_type_t content;
    bool enabled;
    uint32_t epoch, codec_rate;
    char text[256];
} tx_cmd_t;

static QueueHandle_t s_decoded_queue;
static QueueHandle_t s_control_queue;
static QueueHandle_t s_spectrum_queue;
static QueueHandle_t s_tx_queue;
/* The decoder includes large, inactive MFSK/RTTY workspaces; cache its active
 * PSK state in PSRAM and reserve internal SRAM for Viterbi metrics. */
static modem_t s_audio_modem EXT_RAM_BSS_ATTR;
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

#if CONFIG_PSK_AP580_RATE_1200
#define DEFAULT_VOICE_RATE 1200U
#elif CONFIG_PSK_AP580_RATE_2400
#define DEFAULT_VOICE_RATE 2400U
#else
#define DEFAULT_VOICE_RATE 600U
#endif
/* Only audio_task applies controls. TX owns its modulator; UART owns its driver. */
static tx_cmd_t s_settings = {.mode = PSK_MODE_BPSK31, .tone_hz = 2000,
    .dac_volume = 70, .hp_volume = BOARD_AUDIO_OUTPUT_VOLUME, .content = CONTENT_TEXT,
    .codec_rate = DEFAULT_VOICE_RATE, .text = TX_TEXT_DEFAULT};

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

static void publish_settings(modem_t *modem, rx_web_status_t *status)
{
    status->mode = modem_get_psk_mode(modem); status->tone_hz = modem->config.rx_tone_hz;
    status->content_type = s_settings.content; status->content_epoch = s_settings.epoch;
    status->codec_rate = s_settings.codec_rate; status->tx_mode = s_settings.mode;
    status->tx_tone_hz = s_settings.tone_hz; status->display_role = s_display_role;
    status->tx_enabled = s_settings.enabled;
    memcpy(status->tx_text, s_settings.text, sizeof(status->tx_text));
    rx_web_set_status(status);
}
static void begin_session(modem_t *modem)
{
    ++s_settings.epoch;
    psk_rx_reset(&modem->decoder.psk_rx);
    psk_rx_set_binary_sync(&modem->decoder.psk_rx,
                          s_settings.content == CONTENT_AUDIO ? VOICE_SYNC_WORD : 0);
    content_begin(s_settings.content, s_display_role != 0, s_settings.enabled,
                  s_settings.codec_rate, s_settings.epoch);
    (void)xQueueOverwrite(s_tx_queue, &s_settings);
    ESP_LOGI(TAG, "session %lu: %s/%s RX=%s TX=%s rate=%lu",
             (unsigned long)s_settings.epoch, content_name(s_settings.content), s_display_role ? "TX" : "RX",
             psk_mode_name(modem_get_psk_mode(modem)), psk_mode_name(s_settings.mode),
             (unsigned long)s_settings.codec_rate);
}
static void select_content(modem_t *modem, content_type_t type)
{
    const content_type_t old = s_settings.content;
    s_settings.content = type;
    if (type == CONTENT_AUDIO) {
        modem_set_psk_mode(modem, PSK_MODE_2X8PSK1200F); s_settings.mode = PSK_MODE_2X8PSK1200F;
        modem_set_rx_tone(modem, 2000); s_settings.tone_hz = 2000;
    } else if (type == CONTENT_IMAGE) {
        if (!modem_image_mode_allowed(modem_get_psk_mode(modem)))
            modem_set_psk_mode(modem, PSK_MODE_2X8PSK1200F);
        if (!modem_image_mode_allowed(s_settings.mode)) s_settings.mode = PSK_MODE_2X8PSK1200F;
    } else if (old == CONTENT_AUDIO) {
        modem_set_psk_mode(modem, PSK_MODE_BPSK31); s_settings.mode = PSK_MODE_BPSK31;
    }
}
static bool mode_allowed(content_type_t content, psk_mode_t mode)
{
    return (unsigned)mode < PSK_MODE_COUNT &&
        (content != CONTENT_IMAGE || modem_image_mode_allowed(mode)) &&
        (content != CONTENT_AUDIO || mode == PSK_MODE_2X8PSK1200F);
}
static void apply_control_event(modem_t *modem, const ui_event_t *event)
{
    switch (event->type) {
    case UI_EVENT_RX_CONFIG: {
        const content_type_t type = event->content_type < 0 ? s_settings.content : (content_type_t)event->content_type;
        if ((unsigned)type >= CONTENT_COUNT || !mode_allowed(type, event->psk_mode)) return;
        if (type == CONTENT_AUDIO && event->tone_hz != 2000) return;
        if (type != s_settings.content) select_content(modem, type);
        modem_set_psk_mode(modem, event->psk_mode); modem_set_rx_tone(modem, event->tone_hz);
        s_display_role = 0; s_settings.enabled = false;
        break;
    }
    case UI_EVENT_CONTENT_CHANGED:
        if ((unsigned)event->content_type >= CONTENT_COUNT) return;
        select_content(modem, (content_type_t)event->content_type); break;
    case UI_EVENT_VOICE_RATE:
        if (event->codec_rate != 600 && event->codec_rate != 1200 && event->codec_rate != 2400) return;
        s_settings.codec_rate = event->codec_rate; break;
    case UI_EVENT_MODE_CHANGED:
        if (s_settings.content != CONTENT_TEXT) return;
        modem_set_mode(modem, event->mode); break;
    case UI_EVENT_PSK_MODE_CHANGED:
        if (!mode_allowed(s_settings.content, event->psk_mode)) return;
        modem_set_psk_mode(modem, event->psk_mode); break;
    case UI_EVENT_TX_MODE_CHANGED:
        if (!mode_allowed(s_settings.content, event->tx_psk_mode)) return;
        s_settings.mode = event->tx_psk_mode; break;
    case UI_EVENT_TRANSPORT_CHANGED:
        (void)transport_set_mode(event->transport_mode); return;
    case UI_EVENT_TONE_CHANGED:
        if (s_settings.content == CONTENT_AUDIO) return;
        modem_set_rx_tone(modem, event->tone_hz); break;
    case UI_EVENT_TX_TONE:
        if (s_settings.content == CONTENT_AUDIO) return;
        s_settings.tone_hz = event->tone_hz; break;
    case UI_EVENT_DISPLAY_ROLE_CHANGED:
        s_display_role = event->display_role != 0; s_settings.enabled = s_display_role != 0; break;
    case UI_EVENT_TX_STOP:
        s_settings.enabled = false; break;
    case UI_EVENT_TX_TEXT:
        if (s_settings.content != CONTENT_TEXT) return;
        s_settings.mode = event->tx_psk_mode; s_settings.tone_hz = event->tx_tone_hz;
        snprintf(s_settings.text, sizeof(s_settings.text), "%s", event->tx_text);
        s_display_role = 1; s_settings.enabled = true; break;
    case UI_EVENT_OUTPUT_GAIN:
        s_settings.dac_volume = event->output_dac_volume; s_settings.hp_volume = event->output_hp_volume;
        (void)xQueueOverwrite(s_tx_queue, &s_settings); return;
    case UI_EVENT_INPUT_GAIN:
        (void)audio_input_set_gain_db(event->input_gain_db); return;
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
        content_status_t cs; content_get_status(&cs);
        ESP_LOGI(TAG, "content=%s epoch=%lu image=%lu/96 bad=%lu voice=%lu packets=%lu crc=%lu UART=%s in=%lu out=%lu",
                 content_name(s_settings.content), (unsigned long)s_settings.epoch,
                 (unsigned long)cs.image_rows, (unsigned long)cs.image_bad_rows, (unsigned long)cs.voice_frames,
                 (unsigned long)cs.voice_packets, (unsigned long)cs.voice_crc_errors,
                 cs.uart_ready ? "ready" : "unconfigured", (unsigned long)cs.uart_rx_frames, (unsigned long)cs.uart_tx_frames);
        ESP_LOGI(TAG, "ap580 tx_packets=%lu tx_bytes=%lu pending=%lu drops=%lu uart_bytes=%lu badlen=%lu syncdrop=%lu nonvoice=%lu timeouts=%lu rate=%lu",
                 (unsigned long)cs.voice_tx_packets, (unsigned long)cs.voice_tx_bytes,
                 (unsigned long)cs.voice_tx_pending, (unsigned long)cs.queue_drops,
                 (unsigned long)cs.uart_rx_bytes, (unsigned long)cs.uart_bad_length,
                 (unsigned long)cs.uart_sync_drops, (unsigned long)cs.uart_non_voice,
                 (unsigned long)cs.uart_timeouts, (unsigned long)cs.voice_rate);
        rx_web_status_t performance; rx_web_get_status(&performance);
        ESP_LOGI(TAG, "perf load=%u.%u%% max=%u/%uus late=%lu samples/s=%u readerr=%lu image_completed=%lu",
            performance.dsp_load_permille / 10, performance.dsp_load_permille % 10,
            performance.dsp_max_us, performance.block_budget_us, (unsigned long)performance.deadline_misses,
            performance.samples_per_second, (unsigned long)performance.read_errors, (unsigned long)cs.image_completed);
        ESP_LOGI(TAG, "heap internal=%u largest=%u PSRAM=%u fec_reacquires=%lu usb_dropped=%llu",
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned long)rx_debug.fec_reacquires,
            (unsigned long long)transport_usb_dropped_bytes());
        return;
    }
    case UI_EVENT_RX_FEC_BRANCH:
        psk_rx_set_fec_branch_override(&modem->decoder.psk_rx, event->fec_branch_override); return;
    default: return;
    }
    begin_session(modem);
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
    tx_cmd_t config = {.epoch = UINT32_MAX};
    modem_image_tx_stream_t image_stream = {0};
    voice_tx_stream_t voice_stream = {0};
    ESP_LOGI(TAG, "tx task started");
    for (;;) {
        tx_cmd_t next;
        if (xQueueReceive(s_tx_queue, &next, 0) == pdTRUE) {
            (void)audio_output_set_codec_gain((uint8_t)next.dac_volume, (uint8_t)next.hp_volume);
            if (next.epoch != config.epoch) {
                psk_tx_stop(tx); audio_output_clear(); waterfall_stream_init(&s_tx_waterfall);
                modem_image_tx_stream_reset(&image_stream);
                voice_stream = (voice_tx_stream_t){.epoch = next.epoch};
            }
            config = next;
        }
        if (config.enabled && !psk_tx_active(tx)) {
            int rc = 0;
            if (config.content == CONTENT_AUDIO) {
                if (content_voice_tx_ready(config.epoch))
                    rc = psk_tx_start_binary_stream(tx, PSK_MODE_2X8PSK1200F, config.tone_hz,
                                                   content_voice_tx_fetch, &voice_stream);
            } else if (config.content == CONTENT_IMAGE) {
                rc = psk_tx_start_stream(tx, config.mode, config.tone_hz, modem_image_tx_stream_fetch, &image_stream);
            } else rc = psk_tx_start_text_loop(tx, config.mode, config.tone_hz, config.text);
            if (rc != 0) { ESP_LOGE(TAG, "TX start failed: %s", content_name(config.content)); config.enabled = false; }
        }
        if (psk_tx_active(tx)) {
            const size_t n = psk_tx_generate(tx, s_tx_samples, PSK_TX_CHUNK_SAMPLES);
            if (n) {
                (void)audio_output_write(s_tx_samples, n);
                append_waterfall_stream(&s_tx_waterfall, WATERFALL_SOURCE_TX,
                                        s_tx_samples, n, BOARD_CODEC_SAMPLE_RATE_HZ);
            }
        } else vTaskDelay(pdMS_TO_TICKS(2));
        s_tx_active = psk_tx_active(tx); s_tx_queued = audio_output_queued();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static bool web_configure_receiver(psk_mode_t mode, int tone_hz, int content_type)
{
    const ui_event_t event = {.type = UI_EVENT_RX_CONFIG, .psk_mode = mode,
                              .tone_hz = tone_hz, .content_type = content_type};
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
    begin_session(modem);
    publish_settings(modem, &status);
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
            publish_settings(modem, &status);
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
                    psk_rx_set_binary_sync(&modem->decoder.psk_rx,
                        s_settings.content == CONTENT_AUDIO ? VOICE_SYNC_WORD : 0);
                    input_open = 1;
                }
                closed_blocks = 0;
            } else if (closed_blocks < 10) { /* about 53 ms at 256 / 48 kHz */
                ++closed_blocks;
            } else {
                input_open = 0;
            }
            status.block_budget_us = (uint32_t)((uint64_t)count * 1000000 / BOARD_MODEM_SAMPLE_RATE_HZ);
            if (input_open && !s_display_role) {
                const int64_t start = esp_timer_get_time();
                const size_t len = modem_process(modem, s_audio_samples, count,
                                                 s_audio_decoded, sizeof(s_audio_decoded));
                const uint32_t elapsed = (uint32_t)(esp_timer_get_time() - start);
                dsp_sum += elapsed;
                dsp_samples += count;
                if (elapsed > dsp_max) dsp_max = elapsed;
                if (elapsed > status.block_budget_us) ++status.deadline_misses;
                if (s_settings.content == CONTENT_TEXT) publish_decoded_text(s_audio_decoded, len);
                else if (content_receive((const uint8_t *)s_audio_decoded, len))
                    psk_rx_reacquire_binary(&modem->decoder.psk_rx);
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
            publish_settings(modem, &status);
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
    (void)arg;
    static display_state_t state EXT_RAM_BSS_ATTR;
    static spectrum_t spectrum EXT_RAM_BSS_ATTR;
    static spectrum_msg_t spectrum_msg EXT_RAM_BSS_ATTR;
    uint32_t last_sequence = 0, last_source = UINT32_MAX, epoch = UINT32_MAX;
    int64_t last_render = 0;
    for (;;) {
        rx_web_status_t actual;
        rx_web_get_status(&actual);
        if (epoch != actual.content_epoch) {
            epoch = actual.content_epoch;
            state.text_len = 0; state.text[0] = 0;
            xQueueReset(s_decoded_queue); clear_display_spectrum(&spectrum);
            last_sequence = 0; last_source = UINT32_MAX;
        }
        state.selected_psk_mode = actual.mode;
        state.selected_mode = actual.mode == PSK_MODE_BPSK31 ? MODEM_MODE_BPSK31 : MODEM_MODE_PSK63;
        state.selected_tx_psk_mode = actual.tx_mode;
        state.rx_tone_hz = actual.tone_hz; state.tx_tone_hz = actual.tx_tone_hz;
        state.display_role = actual.display_role;
        state.content_type = actual.content_type; state.content_epoch = actual.content_epoch;
        state.codec_rate = actual.codec_rate; memcpy(state.tx_text, actual.tx_text, sizeof(state.tx_text));
        state.transport_mode = transport_get_mode();
        decoded_msg_t msg;
        while (xQueueReceive(s_decoded_queue, &msg, 0) == pdTRUE) display_append_text(&state, msg.text);
        const ui_event_t event = ui_poll(&state);
        if (event.type != UI_EVENT_NONE && xQueueSend(s_control_queue, &event, 0) != pdTRUE)
            ESP_LOGW(TAG, "Control queue full; retry selection");
        const int64_t now = esp_timer_get_time();
        if (now - last_render >= 100000) {
            last_render = now; state.tx_active = s_tx_active; state.tx_queued = s_tx_queued;
            state.input_abs_avg = s_input_abs_avg > 65535 ? 65535U : (uint16_t)s_input_abs_avg;
            state.selected_lane = BOARD_AUDIO_INPUT_CHANNEL; state.update_waterfall = 0;
            if (xQueueReceive(s_spectrum_queue, &spectrum_msg, 0) == pdTRUE &&
                (spectrum_msg.source_key == WATERFALL_SOURCE_TX) == (state.display_role != 0)) {
                if (last_source != spectrum_msg.source_key) { clear_display_spectrum(&spectrum); last_sequence = 0; }
                last_source = spectrum_msg.source_key;
                if (spectrum_msg.sequence != last_sequence) {
                    spectrum_analyze(spectrum_msg.samples, spectrum_msg.count, spectrum_msg.sample_rate_hz, &spectrum);
                    last_sequence = spectrum_msg.sequence; s_waterfall_peak_hz = spectrum.peak_hz;
                    state.update_waterfall = 1;
                }
            }
            display_render(&state, &spectrum);
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
    s_tx_queue = xQueueCreate(1, sizeof(tx_cmd_t));
    ESP_ERROR_CHECK(content_service_init());
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
