#include "audio_input.h"

#include <string.h>

#include "board_config.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "esp_log.h"

static const char *TAG = "audio";
static esp_codec_dev_handle_t s_mic;
static i2s_chan_handle_t s_rx_chan;
static i2c_master_bus_handle_t s_i2c_bus;

static esp_err_t i2c_init(void)
{
    i2c_master_bus_config_t i2c_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = BOARD_AUDIO_I2C_NUM,
        .sda_io_num = BOARD_AUDIO_I2C_SDA,
        .scl_io_num = BOARD_AUDIO_I2C_SCL,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = true,
        },
    };
    return i2c_new_master_bus(&i2c_cfg, &s_i2c_bus);
}

static esp_err_t i2s_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BOARD_AUDIO_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx_chan), TAG, "i2s channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BOARD_CODEC_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_AUDIO_I2S_MCLK,
            .bclk = BOARD_AUDIO_I2S_BCLK,
            .ws = BOARD_AUDIO_I2S_WS,
            .dout = BOARD_AUDIO_I2S_DOUT,
            .din = BOARD_AUDIO_I2S_DIN,
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_chan, &std_cfg), TAG, "i2s std");
    return ESP_OK;
}

static esp_err_t codec_init(void)
{
    ESP_RETURN_ON_ERROR(i2c_init(), TAG, "i2c init");

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = BOARD_AUDIO_I2S_NUM,
        .rx_handle = s_rx_chan,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(data_if != NULL, ESP_ERR_NO_MEM, TAG, "i2s data interface");

    audio_codec_i2c_cfg_t i2c_cfg = {
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = s_i2c_bus,
        .clock_speed_hz = BOARD_AUDIO_I2C_SPEED_HZ,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(ctrl_if != NULL, ESP_ERR_NO_MEM, TAG, "i2c control interface");

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(gpio_if != NULL, ESP_ERR_NO_MEM, TAG, "gpio interface");

    es8311_codec_cfg_t codec_cfg = {
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_ADC,
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .pa_pin = GPIO_NUM_NC,
        .master_mode = false,
        .use_mclk = true,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&codec_cfg);
    ESP_RETURN_ON_FALSE(codec_if != NULL, ESP_ERR_NO_MEM, TAG, "es8311 codec");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_mic = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_mic != NULL, ESP_ERR_NO_MEM, TAG, "codec device");
    return ESP_OK;
}

esp_err_t audio_input_init(void)
{
    ESP_RETURN_ON_ERROR(i2s_init(), TAG, "i2s init");
    ESP_RETURN_ON_ERROR(codec_init(), TAG, "codec init");

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = BOARD_CODEC_SAMPLE_RATE_HZ,
        .channel = 1,
        .bits_per_sample = 16,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
        .mclk_multiple = 256,
    };
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_mic, &fs), TAG, "codec open");
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_in_gain(s_mic, BOARD_AUDIO_INPUT_GAIN_DB), TAG, "codec gain");

    ESP_LOGI(TAG, "%s microphone ready: %d Hz, mono, 16-bit, gain %.1f dB",
             BOARD_NAME, BOARD_CODEC_SAMPLE_RATE_HZ, BOARD_AUDIO_INPUT_GAIN_DB);
    return ESP_OK;
}

size_t audio_input_read(int16_t *samples, size_t max_samples)
{
    if (!samples || max_samples == 0 || !s_mic) {
        return 0;
    }

    if (max_samples > BOARD_AUDIO_BLOCK_SAMPLES) {
        max_samples = BOARD_AUDIO_BLOCK_SAMPLES;
    }

    const int bytes = (int)(max_samples * sizeof(samples[0]));
    const int ret = esp_codec_dev_read(s_mic, samples, bytes);
    if (ret != ESP_CODEC_DEV_OK) {
        memset(samples, 0, max_samples * sizeof(samples[0]));
        ESP_LOGW(TAG, "codec read failed: %d", ret);
        return 0;
    }

    for (size_t i = 0; i < max_samples; ++i) {
        int32_t sample = (int32_t)samples[i] * BOARD_AUDIO_SOFTWARE_GAIN;
        if (sample > INT16_MAX) {
            sample = INT16_MAX;
        } else if (sample < INT16_MIN) {
            sample = INT16_MIN;
        }
        samples[i] = (int16_t)sample;
    }
    return max_samples;
}
