#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "audio_input.h"
#include "board_config.h"
#include "display.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "modem.h"
#include "spectrum.h"
#include "transport.h"
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
} spectrum_msg_t;

static QueueHandle_t s_decoded_queue;
static QueueHandle_t s_control_queue;
static QueueHandle_t s_spectrum_queue;
static modem_t s_audio_modem;
static int16_t s_audio_samples[256];
static char s_audio_decoded[sizeof(((decoded_msg_t *)0)->text)];
static spectrum_msg_t s_audio_spectrum_msg;

static int32_t calc_block_rms(const int16_t *samples, size_t count)
{
    if (count == 0) {
        return 0;
    }
    int64_t sum_sq = 0;
    for (size_t i = 0; i < count; ++i) {
        sum_sq += (int32_t)samples[i] * (int32_t)samples[i];
    }
    return (int32_t)sqrt((double)sum_sq / (double)count);
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
    case UI_EVENT_NONE:
    default:
        break;
    }
}

static void audio_task(void *arg)
{
    const modem_config_t modem_config = {
        .mode = MODEM_MODE_PSK63,
        .psk_mode = PSK_MODE_BPSK63,
        .sample_rate_hz = BOARD_MODEM_SAMPLE_RATE_HZ,
        .rx_tone_hz = 1500,
        .squelch = 30,
    };
    modem_t *modem = &s_audio_modem;
    modem_init(modem, &modem_config);

    int64_t last_status_log_us = 0;
    uint32_t reads_this_second = 0;
    uint32_t samples_this_second = 0;
    uint64_t dsp_us_sum = 0;
    uint32_t dsp_us_max = 0;
    uint32_t dsp_blocks = 0;
    int32_t last_rms = 0;
#if BOARD_MODEM_DEBUG_LOGS
    int last_peak_hz = 0;
#endif
    bool last_active = false;
    bool decoder_active = false;
    int active_hold_blocks = 0;

    ESP_LOGI(TAG, "audio decode task started");

    while (true) {
        ui_event_t event;
        while (xQueueReceive(s_control_queue, &event, 0) == pdTRUE) {
            apply_control_event(modem, &event);
        }

        const size_t count = audio_input_read(s_audio_samples, 256, 50);
        const int64_t now_us = esp_timer_get_time();
        if (count > 0) {
            reads_this_second++;
            samples_this_second += count;

            s_audio_spectrum_msg.count = count > SPECTRUM_MAX_SAMPLES ? SPECTRUM_MAX_SAMPLES : count;
            memcpy(s_audio_spectrum_msg.samples, s_audio_samples,
                   s_audio_spectrum_msg.count * sizeof(s_audio_spectrum_msg.samples[0]));
            (void)xQueueOverwrite(s_spectrum_queue, &s_audio_spectrum_msg);

            last_rms = calc_block_rms(s_audio_samples, count);
            last_active = last_rms >= BOARD_AUDIO_ACTIVE_RMS_MIN;
            if (last_active) {
                active_hold_blocks = 1;
                decoder_active = true;
            } else if (active_hold_blocks > 0) {
                active_hold_blocks--;
            } else if (decoder_active) {
                modem_set_mode(modem, modem_get_mode(modem));
                decoder_active = false;
            }

            if (last_active || active_hold_blocks > 0) {
                const size_t chunk_samples = 64;
                for (size_t pos = 0; pos < count; pos += chunk_samples) {
                    size_t chunk_count = count - pos;
                    if (chunk_count > chunk_samples) {
                        chunk_count = chunk_samples;
                    }
                    const int64_t dsp_start_us = esp_timer_get_time();
                    const size_t decoded_len = modem_process(modem, s_audio_samples + pos, chunk_count,
                                                             s_audio_decoded, sizeof(s_audio_decoded));
                    const uint32_t dsp_us = (uint32_t)(esp_timer_get_time() - dsp_start_us);
                    dsp_us_sum += dsp_us;
                    dsp_blocks++;
                    if (dsp_us > dsp_us_max) {
                        dsp_us_max = dsp_us;
                    }
                    publish_decoded_text(s_audio_decoded, decoded_len);
                    vTaskDelay(pdMS_TO_TICKS(1));
                }
            }
        }

#if BOARD_MODEM_DEBUG_LOGS
        if (now_us - last_status_log_us >= 5000000) {
            last_status_log_us = now_us;
            const psk_rtty_rx_t *rx = &modem->decoder.psk_rtty_rx;
            spectrum_t debug_spectrum;
            spectrum_analyze(s_audio_samples, count, BOARD_MODEM_SAMPLE_RATE_HZ, &debug_spectrum);
            last_peak_hz = debug_spectrum.peak_hz;
            const int nco_hz = (int)lrintf((rx->nco_phase_inc + rx->freq_error) *
                                           (float)rx->config.sample_rate_hz / TWO_PI_F);
            const uint32_t dsp_avg_us = dsp_blocks ? (uint32_t)(dsp_us_sum / dsp_blocks) : 0U;
            const uint32_t dsp_block_us = (uint32_t)((64U * 1000000U) / BOARD_MODEM_SAMPLE_RATE_HZ);
            const uint32_t dsp_load_permille = dsp_block_us ? (dsp_avg_us * 1000U) / dsp_block_us : 0U;
            ESP_LOGI(TAG,
                     "mode=%s rx=%d Hz nco=%d Hz peak=%d Hz rms=%d active=%d reads=%u samples=%u lock=%u sq=%d lvl=%d agc=%d dev=%d pk=%d/%d/%d warm=%d dsp=%u/%uus load=%u.%u%% sym=%u bits=%u z=%u o=%u dd=%u ch=%u unk=%u",
                     modem_mode_name(modem_get_mode(modem)), modem_get_rx_tone(modem),
                     nco_hz, last_peak_hz, (int)last_rms, last_active,
                     (unsigned)reads_this_second, (unsigned)samples_this_second,
                     rx->lock_state, rx->sq_open, rx->sq_level, (int)rx->agc_ave, (int)rx->dev_ave,
                     rx->pk_pos, rx->new_pk_pos, rx->raw_pk_pos, rx->warmup_symbols,
                     (unsigned)dsp_avg_us, (unsigned)dsp_us_max,
                     (unsigned)(dsp_load_permille / 10U), (unsigned)(dsp_load_permille % 10U),
                     (unsigned)rx->debug_symbols, (unsigned)rx->debug_bits,
                     (unsigned)rx->debug_zeros, (unsigned)rx->debug_ones,
                     (unsigned)rx->debug_delims, (unsigned)rx->debug_chars, (unsigned)rx->debug_unknown);
            reads_this_second = 0;
            samples_this_second = 0;
            dsp_us_sum = 0;
            dsp_us_max = 0;
            dsp_blocks = 0;
        }
#else
        (void)now_us;
        if (last_status_log_us == 0) {
            last_status_log_us = 1;
        }
        reads_this_second = 0;
        samples_this_second = 0;
#endif

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static void ui_task(void *arg)
{
    display_state_t display_state = {
        .selected_mode = MODEM_MODE_PSK63,
        .selected_psk_mode = PSK_MODE_BPSK63,
        .transport_mode = TRANSPORT_MODE_USB_SERIAL,
        .rx_tone_hz = 1500,
    };
    spectrum_t spectrum = {0};
    int64_t last_render_us = 0;

    ESP_LOGI(TAG, "ui task started");

    while (true) {
        decoded_msg_t msg;
        while (xQueueReceive(s_decoded_queue, &msg, 0) == pdTRUE) {
            display_append_text(&display_state, msg.text);
        }

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
            break;
        case UI_EVENT_PSK_MODE_CHANGED:
            display_state.selected_psk_mode = event.psk_mode;
            display_state.selected_mode = event.psk_mode == PSK_MODE_BPSK31 ? MODEM_MODE_BPSK31 : MODEM_MODE_PSK63;
            (void)xQueueSend(s_control_queue, &event, 0);
            break;
        case UI_EVENT_TRANSPORT_CHANGED:
            display_state.transport_mode = event.transport_mode;
            (void)xQueueSend(s_control_queue, &event, 0);
            break;
        case UI_EVENT_TONE_CHANGED:
            display_state.rx_tone_hz = event.tone_hz;
            (void)xQueueSend(s_control_queue, &event, 0);
            break;
        case UI_EVENT_NONE:
        default:
            break;
        }

        const int64_t now_us = esp_timer_get_time();
        if (now_us - last_render_us >= 100000) {
            last_render_us = now_us;
            spectrum_msg_t spectrum_msg;
            if (xQueueReceive(s_spectrum_queue, &spectrum_msg, 0) == pdTRUE) {
                spectrum_analyze(spectrum_msg.samples, spectrum_msg.count, BOARD_MODEM_SAMPLE_RATE_HZ, &spectrum);
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

    s_decoded_queue = xQueueCreate(8, sizeof(decoded_msg_t));
    s_control_queue = xQueueCreate(4, sizeof(ui_event_t));
    s_spectrum_queue = xQueueCreate(1, sizeof(spectrum_msg_t));
    ESP_ERROR_CHECK(s_decoded_queue && s_control_queue && s_spectrum_queue ? ESP_OK : ESP_ERR_NO_MEM);

    BaseType_t task_ok = xTaskCreate(audio_task, "psk_audio", 16384, NULL, 8, NULL);
    ESP_ERROR_CHECK(task_ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    task_ok = xTaskCreate(ui_task, "psk_ui", 6144, NULL, 2, NULL);
    ESP_ERROR_CHECK(task_ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_LOGI(TAG, "ESP32-P4 PSK modem started");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
