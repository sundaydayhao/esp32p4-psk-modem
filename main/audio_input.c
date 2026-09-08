#include "audio_input.h"

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "audio_decim.h"
#include "board_config.h"
#include "bsp/esp-bsp.h"
#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "esp_timer.h"

static const char *TAG = "audio";
static esp_codec_dev_handle_t s_mic_dev;
static esp_codec_dev_handle_t s_spk_dev;
static audio_decim_state_t s_decim_state;
static int16_t s_raw_samples[1536];
static uint8_t s_dac_volume = 70;
static uint8_t s_headphone_volume = BOARD_AUDIO_OUTPUT_VOLUME;
static float s_input_gain_db = BOARD_AUDIO_INPUT_GAIN_DB;

#define AUDIO_INPUT_CAPTURE_MAX_SAMPLES (48000U * 40U)
static int16_t *s_capture;
static portMUX_TYPE s_capture_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile size_t s_capture_limit;
static volatile size_t s_capture_count;
static volatile bool s_capture_active;

static void capture_samples(const int16_t *samples, size_t count)
{
    if (!samples || count == 0) return;
    portENTER_CRITICAL(&s_capture_mux);
    if (s_capture_active) {
        const size_t remaining = s_capture_limit - s_capture_count;
        if (count > remaining) count = remaining;
        memcpy(s_capture + s_capture_count, samples, count * sizeof(*samples));
        s_capture_count += count;
        if (s_capture_count == s_capture_limit) s_capture_active = false;
    }
    portEXIT_CRITICAL(&s_capture_mux);
}

esp_err_t audio_input_init(void)
{
    audio_decim_init(&s_decim_state);

    i2s_std_config_t i2s_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BOARD_CODEC_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BOARD_AUDIO_I2S_MCLK,
            .bclk = BOARD_AUDIO_I2S_BCLK,
            .ws = BOARD_AUDIO_I2S_WS,
            .dout = BOARD_AUDIO_I2S_DOUT,
            .din = BOARD_AUDIO_I2S_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    i2s_cfg.clk_cfg.mclk_multiple = BOARD_AUDIO_MCLK_MULTIPLE;

    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "bsp i2c init");
    ESP_RETURN_ON_ERROR(bsp_audio_init(&i2s_cfg), TAG, "bsp audio init");

    s_mic_dev = bsp_audio_codec_microphone_init();
    ESP_RETURN_ON_FALSE(s_mic_dev, ESP_ERR_NO_MEM, TAG, "microphone codec init");
    s_spk_dev = bsp_audio_codec_speaker_init();
    ESP_RETURN_ON_FALSE(s_spk_dev, ESP_ERR_NO_MEM, TAG, "speaker codec init");

    esp_codec_dev_sample_info_t output_fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
        .sample_rate = BOARD_CODEC_SAMPLE_RATE_HZ,
        .mclk_multiple = BOARD_AUDIO_MCLK_MULTIPLE,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_spk_dev, &output_fs) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "open speaker codec");

    esp_codec_dev_sample_info_t input_fs = {
        .bits_per_sample = 16,
        .channel = 2,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(BOARD_AUDIO_INPUT_CHANNEL),
        .sample_rate = BOARD_CODEC_SAMPLE_RATE_HZ,
        .mclk_multiple = BOARD_AUDIO_MCLK_MULTIPLE,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_mic_dev, &input_fs) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "open microphone codec");
    int codec_ret = esp_codec_dev_set_in_gain(s_mic_dev, s_input_gain_db);
    if (codec_ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "set microphone gain failed: %d", codec_ret);
    }
    codec_ret = esp_codec_dev_set_in_mute(s_mic_dev, false);
    if (codec_ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "unmute microphone failed: %d", codec_ret);
    }
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_out_vol(s_spk_dev, s_headphone_volume) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "set speaker volume");
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_out_mute(s_spk_dev, false) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "unmute speaker");

    ESP_LOGI(TAG, "ESP32-P4 BSP audio ready: codec %d Hz, modem %d Hz, input lane %d",
             BOARD_CODEC_SAMPLE_RATE_HZ, BOARD_MODEM_SAMPLE_RATE_HZ, BOARD_AUDIO_INPUT_CHANNEL);
    return ESP_OK;
}

esp_err_t audio_input_set_gain_db(float gain_db)
{
    if (gain_db < 0.0f) {
        gain_db = 0.0f;
    } else if (gain_db > 42.0f) {
        gain_db = 42.0f;
    }
    s_input_gain_db = gain_db;
    if (!s_mic_dev) {
        return ESP_OK;
    }
    return esp_codec_dev_set_in_gain(s_mic_dev, gain_db) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

size_t audio_input_read(int16_t *samples, size_t max_samples, uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (!samples || max_samples == 0 || !s_mic_dev) {
        return 0;
    }

    const size_t decim = BOARD_CODEC_SAMPLE_RATE_HZ / BOARD_MODEM_SAMPLE_RATE_HZ;
    if (decim == 0 || BOARD_CODEC_SAMPLE_RATE_HZ % BOARD_MODEM_SAMPLE_RATE_HZ != 0) {
        ESP_LOGE(TAG, "unsupported codec/modem sample rates");
        return 0;
    }

    if (max_samples > 512) {
        max_samples = 512;
    }

    const size_t raw_count = max_samples * decim;
    const size_t raw_limit = sizeof(s_raw_samples) / sizeof(s_raw_samples[0]);
    const size_t read_count = raw_count < raw_limit ? raw_count : raw_limit;
    const int read_bytes = (int)(read_count * sizeof(s_raw_samples[0]));
    const int read_ret = esp_codec_dev_read(s_mic_dev, s_raw_samples, read_bytes);
    if (read_ret != ESP_CODEC_DEV_OK) {
#if BOARD_AUDIO_DEBUG_LOGS
        ESP_LOGW(TAG, "rx read failed: ret=%d bytes=%d", read_ret, read_bytes);
#endif
        return 0;
    }

    const size_t got_samples = read_count;
    const size_t out_count = audio_decim_process(&s_decim_state, s_raw_samples,
                                                 got_samples, samples, (uint32_t)decim);
    capture_samples(samples, out_count);

#if BOARD_AUDIO_DEBUG_LOGS
    static int64_t last_log_us;
    const int64_t now_us = esp_timer_get_time();
    if (now_us - last_log_us >= 1000000) {
        last_log_us = now_us;
        int16_t s_min = INT16_MAX;
        int16_t s_max = INT16_MIN;
        int64_t s_abs_sum = 0;
        int64_t s_sum_sq = 0;
        for (size_t i = 0; i < out_count; ++i) {
            const int16_t sample = samples[i];
            if (sample < s_min) {
                s_min = sample;
            }
            if (sample > s_max) {
                s_max = sample;
            }
            s_abs_sum += sample < 0 ? -(int32_t)sample : sample;
            s_sum_sq += (int32_t)sample * (int32_t)sample;
        }
        const int32_t rms = out_count ? (int32_t)sqrt((double)s_sum_sq / (double)out_count) : 0;
        ESP_LOGI(TAG, "rx ret=%d bytes=%d raw=%u out=%u S[%d,%d] abs=%d rms=%d",
                 read_ret, read_bytes, (unsigned)got_samples, (unsigned)out_count, s_min, s_max,
                 out_count ? (int)(s_abs_sum / (int64_t)out_count) : 0, (int)rms);
    }
#endif

    return out_count;
}

size_t audio_output_write(const int16_t *samples, size_t count)
{
    if (!samples || count == 0 || !s_spk_dev) {
        return 0;
    }

    const int bytes = (int)(count * sizeof(samples[0]));
    return esp_codec_dev_write(s_spk_dev, (void *)samples, bytes) == ESP_CODEC_DEV_OK ? count : 0;
}

size_t audio_output_queued(void)
{
    return 0;
}

size_t audio_output_capacity(void)
{
    return 4096;
}

void audio_output_clear(void)
{
}

esp_err_t audio_output_set_codec_gain(uint8_t dac_volume, uint8_t headphone_volume)
{
    s_dac_volume = dac_volume;
    s_headphone_volume = headphone_volume > 100 ? 100 : headphone_volume;
    if (!s_spk_dev) {
        return ESP_OK;
    }
    return esp_codec_dev_set_out_vol(s_spk_dev, s_headphone_volume) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t audio_input_capture_start(size_t max_samples)
{
    if (max_samples == 0 || max_samples > AUDIO_INPUT_CAPTURE_MAX_SAMPLES) {
        return ESP_ERR_INVALID_SIZE;
    }

    int16_t *buffer = heap_caps_malloc(max_samples * sizeof(*buffer), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) return ESP_ERR_NO_MEM;
    portENTER_CRITICAL(&s_capture_mux);
    int16_t *old = s_capture;
    s_capture = buffer;
    s_capture_count = 0;
    s_capture_limit = max_samples;
    s_capture_active = true;
    portEXIT_CRITICAL(&s_capture_mux);
    heap_caps_free(old);
    return ESP_OK;
}

void audio_input_capture_stop(void)
{
    portENTER_CRITICAL(&s_capture_mux);
    s_capture_active = false;
    portEXIT_CRITICAL(&s_capture_mux);
}

int audio_input_capture_active(void)
{
    return s_capture_active ? 1 : 0;
}

size_t audio_input_capture_count(void)
{
    return s_capture_count;
}

size_t audio_input_capture_copy(size_t offset, int16_t *samples, size_t max_samples)
{
    if (!samples || !max_samples) return 0;
    portENTER_CRITICAL(&s_capture_mux);
    const size_t count = s_capture_count;
    size_t copied = offset < count ? count - offset : 0;
    if (copied > max_samples) copied = max_samples;
    if (copied) memcpy(samples, &s_capture[offset], copied * sizeof(samples[0]));
    portEXIT_CRITICAL(&s_capture_mux);
    return copied;
}
