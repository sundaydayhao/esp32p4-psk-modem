#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

esp_err_t board_i2c_get_bus(i2c_master_bus_handle_t *bus_handle);
esp_err_t board_pca9557_init(void);
esp_err_t board_pca9557_set_output(uint8_t gpio_bit, bool level);
esp_err_t board_lcd_cs_set(bool level);
esp_err_t board_audio_pa_set(bool level);
