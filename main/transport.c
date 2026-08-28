#include "transport.h"

#include <stdio.h>

#include "esp_check.h"
#include "esp_log.h"
#ifdef ESP_PLATFORM
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#endif

static const char *TAG = "transport";
static transport_mode_t s_mode = TRANSPORT_MODE_USB_SERIAL;

const char *transport_mode_name(transport_mode_t mode)
{
    switch (mode) {
    case TRANSPORT_MODE_USB_SERIAL:
        return "USB";
    case TRANSPORT_MODE_WIFI:
        return "WiFi";
    case TRANSPORT_MODE_BT:
        return "BT";
    default:
        return "?";
    }
}

esp_err_t transport_init(transport_mode_t mode)
{
    return transport_set_mode(mode);
}

esp_err_t transport_set_mode(transport_mode_t mode)
{
    if (mode >= TRANSPORT_MODE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    if (mode == TRANSPORT_MODE_WIFI) {
        ESP_LOGW(TAG, "WiFi transport is unavailable on ESP32-P4; keeping current transport");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (mode == TRANSPORT_MODE_BT) {
        ESP_LOGW(TAG, "BT transport selected, but BLE serial backend is not configured");
    }

    s_mode = mode;
    ESP_LOGI(TAG, "transport=%s", transport_mode_name(mode));
    return ESP_OK;
}

transport_mode_t transport_get_mode(void)
{
    return s_mode;
}

void transport_send(const uint8_t *data, size_t len)
{
    if (!data || len == 0) {
        return;
    }

    switch (s_mode) {
    case TRANSPORT_MODE_USB_SERIAL:
#ifdef ESP_PLATFORM
        (void)usb_serial_jtag_write_bytes(data, len, pdMS_TO_TICKS(20));
#else
        fwrite(data, 1, len, stdout);
        fflush(stdout);
#endif
        break;
    case TRANSPORT_MODE_BT:
    case TRANSPORT_MODE_WIFI:
    default:
        break;
    }
}
