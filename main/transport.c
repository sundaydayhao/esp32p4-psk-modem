#include "transport.h"
#include "rx_history.h"
#include <stdbool.h>
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "transport";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static rx_history_t s_history;
static transport_mode_t s_mode = TRANSPORT_MODE_USB_SERIAL;
static uint64_t s_usb_dropped;
static uint32_t s_epoch;

const char *transport_mode_name(transport_mode_t mode)
{
    switch (mode) {
    case TRANSPORT_MODE_USB_SERIAL: return "USB+AP";
    case TRANSPORT_MODE_WIFI: return "AP";
    case TRANSPORT_MODE_BT: return "BT";
    default: return "?";
    }
}

static void usb_task(void *arg)
{
    (void)arg;
    uint64_t cursor = 0;
    uint8_t bytes[256];
    for (;;) {
        uint64_t lost;
        const size_t n = transport_read(&cursor, bytes, sizeof(bytes), &lost, NULL);
        const bool enabled = transport_get_mode() == TRANSPORT_MODE_USB_SERIAL;
        int written = 0;
        if (n && enabled) written = usb_serial_jtag_write_bytes(bytes, n, pdMS_TO_TICKS(20));
        if (enabled) {
            portENTER_CRITICAL(&s_lock);
            s_usb_dropped += lost + n - (written > 0 ? (size_t)written : 0);
            portEXIT_CRITICAL(&s_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t transport_init(transport_mode_t mode)
{
    esp_err_t err = transport_set_mode(mode);
    if (err != ESP_OK) return err;
    return xTaskCreatePinnedToCore(usb_task, "rx_usb", 3072, NULL, 2, NULL, 1) == pdPASS ?
        ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t transport_set_mode(transport_mode_t mode)
{
    if ((unsigned)mode >= TRANSPORT_MODE_COUNT) return ESP_ERR_INVALID_ARG;
    if (mode == TRANSPORT_MODE_BT) return ESP_ERR_NOT_SUPPORTED;
    portENTER_CRITICAL(&s_lock);
    s_mode = mode;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "transport=%s; AP receive history always enabled", transport_mode_name(mode));
    return ESP_OK;
}

transport_mode_t transport_get_mode(void)
{
    portENTER_CRITICAL(&s_lock);
    transport_mode_t mode = s_mode;
    portEXIT_CRITICAL(&s_lock);
    return mode;
}

void transport_send(const uint8_t *data, size_t len)
{
    portENTER_CRITICAL(&s_lock);
    rx_history_append(&s_history, data, len);
    portEXIT_CRITICAL(&s_lock);
}

void transport_begin_session(uint32_t epoch)
{
    portENTER_CRITICAL(&s_lock);
    s_epoch = epoch; s_history.session_start = s_history.total;
    portEXIT_CRITICAL(&s_lock);
}

size_t transport_read(uint64_t *cursor, uint8_t *out, size_t capacity, uint64_t *lost, uint32_t *epoch)
{
    portENTER_CRITICAL(&s_lock);
    const size_t n = rx_history_read(&s_history, cursor, out, capacity, lost);
    if (epoch) *epoch = s_epoch;
    portEXIT_CRITICAL(&s_lock);
    return n;
}

uint64_t transport_received_bytes(void)
{
    portENTER_CRITICAL(&s_lock);
    const uint64_t n = s_history.total;
    portEXIT_CRITICAL(&s_lock);
    return n;
}

uint64_t transport_usb_dropped_bytes(void)
{
    portENTER_CRITICAL(&s_lock);
    const uint64_t n = s_usb_dropped;
    portEXIT_CRITICAL(&s_lock);
    return n;
}
