#include "board_i2c.h"

#include "board_config.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "board_i2c";
static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_pca9557_dev;
static bool s_pca9557_ready;

#define PCA9557_OUTPUT_PORT 0x01
#define PCA9557_CONFIGURATION_PORT 0x03

esp_err_t board_i2c_get_bus(i2c_master_bus_handle_t *bus_handle)
{
    ESP_RETURN_ON_FALSE(bus_handle, ESP_ERR_INVALID_ARG, TAG, "bus_handle is NULL");

    if (!s_i2c_bus) {
        const i2c_master_bus_config_t conf = {
            .i2c_port = BOARD_TOUCH_I2C_PORT,
            .sda_io_num = BOARD_TOUCH_PIN_SDA,
            .scl_io_num = BOARD_TOUCH_PIN_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags = {
                .enable_internal_pullup = true,
            },
        };
        ESP_RETURN_ON_ERROR(i2c_new_master_bus(&conf, &s_i2c_bus), TAG, "i2c bus");
    }

    *bus_handle = s_i2c_bus;
    return ESP_OK;
}

static esp_err_t pca9557_write_reg(uint8_t reg_addr, uint8_t data)
{
    uint8_t write_buf[2] = {reg_addr, data};
    return i2c_master_transmit(s_pca9557_dev, write_buf, sizeof(write_buf), 1000);
}

static esp_err_t pca9557_read_reg(uint8_t reg_addr, uint8_t *data)
{
    return i2c_master_transmit_receive(s_pca9557_dev, &reg_addr, 1, data, 1, 1000);
}

esp_err_t board_pca9557_init(void)
{
    if (!s_pca9557_dev) {
        i2c_master_bus_handle_t bus = NULL;
        ESP_RETURN_ON_ERROR(board_i2c_get_bus(&bus), TAG, "i2c bus");
        const i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = BOARD_PCA9557_ADDR,
            .scl_speed_hz = BOARD_I2C_SPEED_HZ,
        };
        ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &s_pca9557_dev), TAG, "pca9557 add device");
    }
    if (!s_pca9557_ready) {
        ESP_RETURN_ON_ERROR(pca9557_write_reg(PCA9557_OUTPUT_PORT, 0x05), TAG, "pca9557 output default");
        ESP_RETURN_ON_ERROR(pca9557_write_reg(PCA9557_CONFIGURATION_PORT, 0xf8), TAG, "pca9557 config");
        s_pca9557_ready = true;
    }
    return ESP_OK;
}

esp_err_t board_pca9557_set_output(uint8_t gpio_bit, bool level)
{
    ESP_RETURN_ON_ERROR(board_pca9557_init(), TAG, "pca9557 init");

    uint8_t data = 0;
    ESP_RETURN_ON_ERROR(pca9557_read_reg(PCA9557_OUTPUT_PORT, &data), TAG, "pca9557 read output");
    if (level) {
        data |= gpio_bit;
    } else {
        data &= (uint8_t)~gpio_bit;
    }
    return pca9557_write_reg(PCA9557_OUTPUT_PORT, data);
}

esp_err_t board_lcd_cs_set(bool level)
{
    return board_pca9557_set_output(BOARD_PCA9557_LCD_CS_BIT, level);
}

esp_err_t board_audio_pa_set(bool level)
{
    return board_pca9557_set_output(BOARD_PCA9557_PA_EN_BIT, level);
}
