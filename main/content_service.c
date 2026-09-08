#include "content_service.h"
#include <string.h>
#include "ap580_playout.h"
#include "uart_tx_pump.h"
#include "transport.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

typedef struct { voice_packet_t packet; uint32_t epoch; } packet_item_t;
typedef struct { uint8_t data[16]; uint32_t epoch, rate; } frame_item_t;
typedef struct { content_type_t type; bool transmit, active; uint32_t rate, epoch; } session_t;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_packets, s_frames;
static session_t s_session;
static content_status_t s_status;
static uint8_t s_pixels[MODEM_IMAGE_PIXEL_COUNT];
static modem_image_rx_parser_t s_image_rx;
static voice_wire_rx_t s_wire_rx;
static uint32_t s_sync_drops;

const char *content_name(content_type_t type)
{
    static const char *names[] = {"Text", "Image", "Audio"};
    return (unsigned)type < CONTENT_COUNT ? names[type] : "?";
}
static session_t session_get(void)
{
    portENTER_CRITICAL(&s_lock);
    session_t session = s_session;
    portEXIT_CRITICAL(&s_lock);
    return session;
}
static void count_drop(void)
{
    portENTER_CRITICAL(&s_lock); ++s_status.queue_drops; portEXIT_CRITICAL(&s_lock);
}
void content_get_status(content_status_t *status)
{
    portENTER_CRITICAL(&s_lock); *status = s_status; portEXIT_CRITICAL(&s_lock);
    status->voice_tx_pending = s_packets ? uxQueueMessagesWaiting(s_packets) : 0;
}
void content_copy_image(uint8_t *pixels, content_status_t *status)
{
    /* Bounded memory copy only; rendering and network writes happen after unlocking. */
    portENTER_CRITICAL(&s_lock);
    memcpy(pixels, s_pixels, sizeof(s_pixels));
    *status = s_status;
    portEXIT_CRITICAL(&s_lock);
}
void content_begin(content_type_t type, bool transmit, bool active, uint32_t rate, uint32_t epoch)
{
    modem_image_rx_parser_init(&s_image_rx);
    memset(&s_wire_rx, 0, sizeof(s_wire_rx)); s_sync_drops = 0;
    xQueueReset(s_packets); xQueueReset(s_frames);
    transport_begin_session(epoch);
    portENTER_CRITICAL(&s_lock);
    const bool ready = s_status.uart_ready;
    const uint32_t version = s_status.image_version + 1;
    memset(&s_status, 0, sizeof(s_status));
    s_status.uart_ready = ready; s_status.epoch = epoch;
    s_status.voice_rate = rate; s_status.image_version = version;
    s_session = (session_t){type, transmit, active, rate, epoch};
    if (type == CONTENT_IMAGE && transmit) {
        memcpy(s_pixels, g_modem_test_image_rgb332, sizeof(s_pixels));
        s_status.image_rows = MODEM_IMAGE_HEIGHT;
    } else memset(s_pixels, 0, sizeof(s_pixels));
    portEXIT_CRITICAL(&s_lock);
}

static void image_start(void *user)
{
    (void)user;
    portENTER_CRITICAL(&s_lock);
    memset(s_pixels, 0, sizeof(s_pixels)); s_status.image_rows = 0;
    ++s_status.image_version;
    portEXIT_CRITICAL(&s_lock);
}
static void image_row(uint16_t row, const uint8_t *pixels, size_t count, uint16_t complete, void *user)
{
    (void)user;
    if (row >= MODEM_IMAGE_HEIGHT || count != MODEM_IMAGE_WIDTH) return;
    portENTER_CRITICAL(&s_lock);
    memcpy(s_pixels + row * MODEM_IMAGE_WIDTH, pixels, count);
    s_status.image_rows = complete; ++s_status.image_version;
    portEXIT_CRITICAL(&s_lock);
}
static void receive_voice(const uint8_t *frames, unsigned count, uint32_t rate, void *user)
{
    (void)user;
    const session_t session = session_get();
    if (!rate) rate = session.rate;
    portENTER_CRITICAL(&s_lock);
    const bool ready = s_status.uart_ready;
    s_status.voice_rate = rate;
    memcpy(s_status.last_voice_frame, frames + (count - 1) * VOICE_FRAME_BYTES, VOICE_FRAME_BYTES);
    portEXIT_CRITICAL(&s_lock);
    for (unsigned i = 0; ready && i < count; ++i) {
        frame_item_t frame = {.epoch = session.epoch, .rate = rate};
        memcpy(frame.data, frames + i * VOICE_FRAME_BYTES, VOICE_FRAME_BYTES);
        if (xQueueSend(s_frames, &frame, 0) != pdTRUE) {
            frame_item_t old;
            (void)xQueueReceive(s_frames, &old, 0); count_drop();
            if (xQueueSend(s_frames, &frame, 0) != pdTRUE) count_drop();
        }
    }
    transport_send(frames, count * VOICE_FRAME_BYTES);
}
bool content_receive(const uint8_t *data, size_t len)
{
    const session_t session = session_get();
    if (session.transmit) return false;
    if (session.type == CONTENT_IMAGE) {
        modem_image_rx_parser_feed(&s_image_rx, (const char *)data, len, image_start, image_row, NULL);
        transport_send(data, len);
        portENTER_CRITICAL(&s_lock);
        s_status.image_bad_rows = s_image_rx.total_invalid_rows;
        s_status.image_completed = s_image_rx.completed_frames;
        portEXIT_CRITICAL(&s_lock);
    } else if (session.type == CONTENT_AUDIO) {
        const uint32_t packets = s_wire_rx.packets, errors = s_wire_rx.crc_errors;
        voice_wire_feed(&s_wire_rx, data, len, receive_voice, NULL);
        if (packets != s_wire_rx.packets || errors != s_wire_rx.crc_errors) s_sync_drops = s_wire_rx.sync_drops;
        const bool reacquire = s_wire_rx.sync_drops - s_sync_drops >= 128;
        portENTER_CRITICAL(&s_lock);
        s_status.voice_packets = s_wire_rx.packets;
        s_status.voice_frames = s_wire_rx.frames;
        s_status.voice_crc_errors = s_wire_rx.crc_errors;
        if (reacquire) ++s_status.voice_resyncs;
        portEXIT_CRITICAL(&s_lock);
        if (reacquire) { s_sync_drops = s_wire_rx.sync_drops; s_wire_rx.state = s_wire_rx.sync = 0; }
        return reacquire;
    }
    return false;
}

int content_voice_tx_fetch(void *user)
{
    voice_tx_stream_t *stream = user;
    if (stream->offset == stream->packet.len) {
        packet_item_t item;
        while (xQueueReceive(s_packets, &item, 0) == pdTRUE) {
            if (item.epoch != stream->epoch) continue;
            stream->packet = item.packet; stream->offset = 0; break;
        }
    }
    if (stream->offset >= stream->packet.len) return -1;
    const int byte = stream->packet.data[stream->offset++];
    if (stream->offset == stream->packet.len) {
        portENTER_CRITICAL(&s_lock);
        if (s_status.epoch == stream->epoch) {
            ++s_status.voice_tx_packets;
            s_status.voice_tx_bytes += stream->packet.len;
        }
        portEXIT_CRITICAL(&s_lock);
    }
    return byte;
}

static void enqueue_packet(const voice_packet_t *packet, void *user)
{
    const session_t *session = user;
    packet_item_t item = {.packet = *packet, .epoch = session->epoch};
    if (xQueueSend(s_packets, &item, 0) != pdTRUE) count_drop();
}
static size_t uart_try_write(void *user, const uint8_t *data, size_t len)
{
    (void)user;
    const int n = uart_tx_chars(UART_NUM_1, (const char *)data, len);
    return n > 0 ? (size_t)n : 0;
}
static int uart_idle(void *user) { (void)user; return uart_wait_tx_done(UART_NUM_1, 0) == ESP_OK; }

static void uart_task(void *arg)
{
    (void)arg;
    voice_uart_rx_t rx = {0};
    ap580_playout_t playout;
    uart_tx_pump_t pump = {0};
    uint32_t applied_epoch = UINT32_MAX;
    uint32_t uart_rx_bytes = 0;
    ap580_playout_reset(&playout, AP580_CODEC_RATE_DEFAULT, 0);
    for (;;) {
        session_t session = session_get();
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (applied_epoch != session.epoch) {
            applied_epoch = session.epoch;
            uart_rx_bytes = 0;
            memset(&rx, 0, sizeof(rx)); uart_tx_pump_cancel(&pump);
            (void)uart_wait_tx_done(UART_NUM_1, pdMS_TO_TICKS(UART_TX_PUMP_TIMEOUT_MS));
            memset(&pump, 0, sizeof(pump));
            (void)uart_flush_input(UART_NUM_1);
            ap580_playout_reset(&playout, session.rate, now);
        }
        uint8_t bytes[64];
        const int n = uart_read_bytes(UART_NUM_1, bytes, sizeof(bytes), 0);
        if (n > 0) uart_rx_bytes += (uint32_t)n;
        if (n > 0 && session.type == CONTENT_AUDIO && session.transmit && session.active)
            voice_uart_feed(&rx, bytes, (size_t)n, session.rate, now, enqueue_packet, &session);
        if (session.type == CONTENT_AUDIO && session.transmit && session.active)
            voice_uart_flush_due(&rx, now, enqueue_packet, &session);
        const uart_tx_pump_result_t result = uart_tx_pump_step(&pump, now, uart_try_write, uart_idle, NULL);
        if (result == UART_TX_PUMP_TIMEOUT) ap580_playout_reset(&playout, session.rate, now);
        if (!pump.length && session.type == CONTENT_AUDIO && !session.transmit && ap580_playout_due(&playout, now)) {
            frame_item_t frame;
            bool fresh = false;
            while (xQueueReceive(s_frames, &frame, 0) == pdTRUE) {
                if (frame.epoch == session.epoch) { fresh = true; break; }
            }
            if (fresh && playout.rate != frame.rate) ap580_playout_reset(&playout, frame.rate, now);
            uint8_t out[16];
            if (ap580_playout_next(&playout, fresh ? frame.data : NULL, now, out) == AP580_PLAYOUT_VOICE)
                (void)uart_tx_pump_begin(&pump, out, sizeof(out), now);
        }
        portENTER_CRITICAL(&s_lock);
        if (s_status.epoch == session.epoch) {
            s_status.uart_rx_frames = rx.accepted; s_status.uart_bad_length = rx.bad_length;
            s_status.uart_rx_bytes = uart_rx_bytes;
            s_status.uart_sync_drops = rx.sync_drops; s_status.uart_non_voice = rx.non_voice;
            s_status.uart_tx_frames = pump.completed_frames; s_status.uart_timeouts = pump.timeouts;
            if (session.transmit && rx.accepted) memcpy(s_status.last_voice_frame, rx.uart, 16);
        }
        portEXIT_CRITICAL(&s_lock);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

esp_err_t content_service_init(void)
{
    s_packets = xQueueCreate(8, sizeof(packet_item_t));
    s_frames = xQueueCreate(20, sizeof(frame_item_t));
    if (!s_packets || !s_frames) return ESP_ERR_NO_MEM;
#if CONFIG_PSK_AP580_UART_ENABLED
    if (!GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_PSK_AP580_TX_GPIO) ||
        !GPIO_IS_VALID_GPIO(CONFIG_PSK_AP580_RX_GPIO) ||
        CONFIG_PSK_AP580_TX_GPIO == CONFIG_PSK_AP580_RX_GPIO) return ESP_ERR_INVALID_ARG;
    const uart_config_t cfg = {.baud_rate = CONFIG_PSK_AP580_UART_BAUD,
        .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT};
    esp_err_t err = uart_param_config(UART_NUM_1, &cfg);
    if (err == ESP_OK) err = uart_set_pin(UART_NUM_1, CONFIG_PSK_AP580_TX_GPIO,
        CONFIG_PSK_AP580_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err == ESP_OK) err = uart_driver_install(UART_NUM_1, 2048, 0, 0, NULL, 0);
    if (err != ESP_OK) return err;
    s_status.uart_ready = true;
    if (xTaskCreatePinnedToCore(uart_task, "ap580_uart", 4096, NULL, 5, NULL, 1) != pdPASS)
        return ESP_ERR_NO_MEM;
    ESP_LOGI("content", "AP580 UART1 ready: TX=%d RX=%d baud=%d", CONFIG_PSK_AP580_TX_GPIO,
             CONFIG_PSK_AP580_RX_GPIO, CONFIG_PSK_AP580_UART_BAUD);
#else
    (void)uart_task;
    ESP_LOGI("content", "AP580 UART not configured; Audio packets can be received and inspected");
#endif
    return ESP_OK;
}
