#include "rx_web.h"
#include "transport.h"
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "rx-web";
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static rx_web_status_t s_status = {.mode = PSK_MODE_BPSK31, .tone_hz = PSK_DEFAULT_TONE_HZ};
static rx_web_config_cb_t s_config_cb;
static char s_boot_id[17];
extern const char rx_html_start[] asm("_binary_rx_html_start");
extern const char rx_html_end[] asm("_binary_rx_html_end");

void rx_web_set_status(const rx_web_status_t *status)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status = *status;
    portEXIT_CRITICAL(&s_status_lock);
}

void rx_web_get_status(rx_web_status_t *status)
{
    portENTER_CRITICAL(&s_status_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_status_lock);
}

static void no_cache(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "X-Boot-Id", s_boot_id);
}

static esp_err_t page_handler(httpd_req_t *req)
{
    no_cache(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, rx_html_start, rx_html_end - rx_html_start - 1);
}

static esp_err_t receive_handler(httpd_req_t *req)
{
    char query[80], value[32];
    uint64_t cursor = 0, lost;
    if (httpd_req_get_url_query_len(req)) {
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
            httpd_query_key_value(query, "cursor", value, sizeof(value)) != ESP_OK || !value[0])
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid cursor");
        for (const char *p = value; *p; ++p)
            if (*p < '0' || *p > '9') return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid cursor");
        errno = 0;
        cursor = strtoull(value, NULL, 10);
        if (errno == ERANGE) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Cursor overflow");
    }
    uint8_t bytes[1024];
    uint32_t epoch;
    const size_t n = transport_read(&cursor, bytes, sizeof(bytes), &lost, &epoch);
    char next[24], skipped[24], generation[16];
    snprintf(next, sizeof(next), "%" PRIu64, cursor);
    snprintf(skipped, sizeof(skipped), "%" PRIu64, lost);
    snprintf(generation, sizeof(generation), "%" PRIu32, epoch);
    no_cache(req);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "X-Next-Cursor", next);
    httpd_resp_set_hdr(req, "X-Lost-Bytes", skipped);
    httpd_resp_set_hdr(req, "X-Content-Epoch", generation);
    return httpd_resp_send(req, (const char *)bytes, n);
}

static esp_err_t send_json(httpd_req_t *req, cJSON *json)
{
    if (!json) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    char *body = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!body) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    no_cache(req);
    httpd_resp_set_type(req, "application/json");
    const esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    return err;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    rx_web_status_t s;
    rx_web_get_status(&s);
    content_status_t cs;
    content_get_status(&cs);
    if (cs.epoch != s.content_epoch) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "Content changing; retry");
    }
    const psk_mode_info_t *mode = psk_mode_info(s.mode);
    cJSON *json = cJSON_CreateObject();
    if (!json) return send_json(req, NULL);
    cJSON_AddStringToObject(json, "boot_id", s_boot_id);
    cJSON_AddNumberToObject(json, "content_type", s.content_type);
    cJSON_AddNumberToObject(json, "content_epoch", s.content_epoch);
    cJSON_AddStringToObject(json, "role", s.display_role ? "TX" : "RX");
    cJSON_AddNumberToObject(json, "image_rows", cs.image_rows);
    cJSON_AddNumberToObject(json, "image_version", cs.image_version);
    cJSON_AddNumberToObject(json, "image_bad_rows", cs.image_bad_rows);
    cJSON_AddNumberToObject(json, "voice_rate", cs.voice_rate);
    cJSON_AddNumberToObject(json, "voice_packets", cs.voice_packets);
    cJSON_AddNumberToObject(json, "voice_frames", cs.voice_frames);
    cJSON_AddNumberToObject(json, "voice_crc_errors", cs.voice_crc_errors);
    cJSON_AddNumberToObject(json, "uart_rx_frames", cs.uart_rx_frames);
    cJSON_AddNumberToObject(json, "uart_rx_bytes", cs.uart_rx_bytes);
    cJSON_AddNumberToObject(json, "uart_bad_length", cs.uart_bad_length);
    cJSON_AddNumberToObject(json, "uart_sync_drops", cs.uart_sync_drops);
    cJSON_AddNumberToObject(json, "uart_non_voice", cs.uart_non_voice);
    cJSON_AddNumberToObject(json, "voice_tx_packets", cs.voice_tx_packets);
    cJSON_AddNumberToObject(json, "voice_tx_bytes", cs.voice_tx_bytes);
    cJSON_AddNumberToObject(json, "voice_tx_pending", cs.voice_tx_pending);
    cJSON_AddNumberToObject(json, "uart_tx_frames", cs.uart_tx_frames);
    cJSON_AddNumberToObject(json, "uart_timeouts", cs.uart_timeouts);
    cJSON_AddNumberToObject(json, "queue_drops", cs.queue_drops);
    cJSON_AddBoolToObject(json, "uart_ready", cs.uart_ready);
    char frame[49];
    for (unsigned i = 0; i < 16; ++i) snprintf(frame + i * 3, 4, "%02X ", cs.last_voice_frame[i]);
    cJSON_AddStringToObject(json, "last_voice_frame", frame);
    cJSON_AddStringToObject(json, "mode", mode->name);
    cJSON_AddNumberToObject(json, "mode_id", s.mode);
    cJSON_AddNumberToObject(json, "tone_hz", s.tone_hz);
    cJSON_AddNumberToObject(json, "baud", mode->baud);
    cJSON_AddNumberToObject(json, "carriers", mode->carriers);
    const double coded = mode->baud * mode->bits_per_symbol * mode->carriers;
    cJSON_AddNumberToObject(json, "information_bps", coded * (mode->fec ? (mode->punctured ? 2.0/3.0 : 0.5) : 1.0));
    cJSON_AddNumberToObject(json, "lock_state", s.lock_state);
    cJSON_AddNumberToObject(json, "input_level", s.input_level);
    cJSON_AddNumberToObject(json, "samples_per_second", s.samples_per_second);
    cJSON_AddNumberToObject(json, "dsp_load_percent", s.dsp_load_permille / 10.0);
    cJSON_AddNumberToObject(json, "dsp_max_us", s.dsp_max_us);
    cJSON_AddNumberToObject(json, "block_budget_us", s.block_budget_us);
    cJSON_AddNumberToObject(json, "deadline_misses", s.deadline_misses);
    cJSON_AddNumberToObject(json, "read_errors", s.read_errors);
    cJSON_AddNumberToObject(json, "received_bytes", (double)transport_received_bytes());
    cJSON_AddNumberToObject(json, "usb_dropped_bytes", (double)transport_usb_dropped_bytes());
    cJSON_AddNumberToObject(json, "internal_heap_min", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(json, "uptime_ms", (double)(esp_timer_get_time() / 1000));
    return send_json(req, json);
}

static esp_err_t modes_handler(httpd_req_t *req)
{
    cJSON *json = cJSON_CreateArray();
    if (!json) return send_json(req, NULL);
    for (size_t i = 0; i < psk_mode_count(); ++i) {
        cJSON *item = cJSON_CreateObject();
        if (!item) { cJSON_Delete(json); return send_json(req, NULL); }
        cJSON_AddNumberToObject(item, "id", i);
        cJSON_AddStringToObject(item, "name", psk_mode_info_at(i)->name);
        cJSON_AddBoolToObject(item, "image_allowed", modem_image_mode_allowed((psk_mode_t)i));
        cJSON_AddItemToArray(json, item);
    }
    return send_json(req, json);
}

static esp_err_t config_handler(httpd_req_t *req)
{
    char body[129], content_type[48];
    /* JSON prevents cross-origin HTML form submissions on the AP. */
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK ||
        strcmp(content_type, "application/json") != 0 || req->content_len == 0 || req->content_len >= sizeof(body))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Expected small JSON request");
    size_t used = 0;
    while (used < req->content_len) {
        int n = httpd_req_recv(req, body + used, req->content_len - used);
        if (n <= 0) return ESP_FAIL;
        used += n;
    }
    body[used] = 0;
    cJSON *json = cJSON_ParseWithLengthOpts(body, used + 1, NULL, true);
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(json, "mode");
    const cJSON *tone = cJSON_GetObjectItemCaseSensitive(json, "tone_hz");
    const cJSON *content = cJSON_GetObjectItemCaseSensitive(json, "content_type");
    rx_web_status_t current;
    rx_web_get_status(&current);
    const int selected = content && cJSON_IsNumber(content) ? content->valueint : current.content_type;
    const bool valid = (!content || (cJSON_IsNumber(content) && content->valuedouble == selected)) &&
        selected >= 0 && selected < CONTENT_COUNT && cJSON_IsNumber(mode) && cJSON_IsNumber(tone) &&
        isfinite(mode->valuedouble) && isfinite(tone->valuedouble) &&
        mode->valuedouble >= 0 && mode->valuedouble < PSK_MODE_COUNT &&
        mode->valuedouble == mode->valueint && tone->valuedouble == tone->valueint &&
        tone->valueint >= 100 && tone->valueint <= 3500 &&
        (selected != CONTENT_IMAGE || modem_image_mode_allowed((psk_mode_t)mode->valueint)) &&
        (selected != CONTENT_AUDIO || (mode->valueint == PSK_MODE_2X8PSK1200F && tone->valueint == 2000));
    if (!valid) {
        cJSON_Delete(json);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid mode or tone (100-3500 Hz)");
    }
    const bool queued = s_config_cb && s_config_cb((psk_mode_t)mode->valueint, tone->valueint, selected);
    cJSON_Delete(json);
    if (!queued) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "Receiver busy; retry");
    }
    httpd_resp_set_status(req, "202 Accepted");
    no_cache(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"queued\":true}");
}

static void put_le32(uint8_t *out, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) out[i] = (uint8_t)(value >> (i * 8));
}

static esp_err_t image_handler(httpd_req_t *req)
{
    enum { BMP_BYTES = 54 + 96 * 96 * 3 };
    uint8_t *storage = heap_caps_malloc(BMP_BYTES + MODEM_IMAGE_PIXEL_COUNT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!storage) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No image memory");
    uint8_t *pixels = storage + BMP_BYTES;
    content_status_t cs;
    content_copy_image(pixels, &cs);
    memset(storage, 0, 54);
    storage[0] = 'B'; storage[1] = 'M';
    put_le32(storage + 2, BMP_BYTES); put_le32(storage + 10, 54); put_le32(storage + 14, 40);
    put_le32(storage + 18, 96); put_le32(storage + 22, 96);
    storage[26] = 1; storage[28] = 24; put_le32(storage + 34, BMP_BYTES - 54);
    for (unsigned y = 0; y < 96; ++y) for (unsigned x = 0; x < 96; ++x) {
        const uint8_t p = pixels[(95 - y) * 96 + x];
        uint8_t *rgb = storage + 54 + (y * 96 + x) * 3;
        rgb[0] = (p & 3) * 255 / 3; rgb[1] = ((p >> 2) & 7) * 255 / 7; rgb[2] = (p >> 5) * 255 / 7;
    }
    char epoch[16], version[16];
    snprintf(epoch, sizeof(epoch), "%" PRIu32, cs.epoch);
    snprintf(version, sizeof(version), "%" PRIu32, cs.image_version);
    no_cache(req);
    httpd_resp_set_hdr(req, "X-Content-Epoch", epoch);
    httpd_resp_set_hdr(req, "X-Image-Version", version);
    httpd_resp_set_type(req, "image/bmp");
    const esp_err_t result = httpd_resp_send(req, (const char *)storage, BMP_BYTES);
    free(storage);
    return result;
}

static esp_err_t start_ap(void)
{
    const size_t ssid_len = strlen(CONFIG_PSK_AP_SSID), pass_len = strlen(CONFIG_PSK_AP_PASSWORD);
    if (!ssid_len || ssid_len > 32 || pass_len < 8 || pass_len > 63) return ESP_ERR_INVALID_ARG;
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) return err; /* Preserve NVS on version/space errors. */
    if ((err = esp_netif_init()) != ESP_OK) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    if (!esp_netif_create_default_wifi_ap()) return ESP_ERR_NO_MEM;
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if ((err = esp_wifi_init(&init)) != ESP_OK) return err;
    if ((err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) return err;
    wifi_config_t config = {0};
    memcpy(config.ap.ssid, CONFIG_PSK_AP_SSID, ssid_len);
    memcpy(config.ap.password, CONFIG_PSK_AP_PASSWORD, pass_len);
    config.ap.ssid_len = ssid_len;
    config.ap.channel = CONFIG_PSK_AP_CHANNEL;
    config.ap.max_connection = 4;
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    if ((err = esp_wifi_set_mode(WIFI_MODE_AP)) != ESP_OK) return err;
    if ((err = esp_wifi_set_config(WIFI_IF_AP, &config)) != ESP_OK) return err;
    return esp_wifi_start();
}

static void web_task(void *arg)
{
    (void)arg;
    esp_err_t err = start_ap();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AP startup failed: %s; check C6 ESP-Hosted firmware/SDIO. RX continues.", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.core_id = 1;
    config.task_priority = 3;
    config.stack_size = 8192;
    config.max_open_sockets = 5;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 2;
    config.send_wait_timeout = 2;
    httpd_handle_t server = NULL;
    err = httpd_start(&server, &config);
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page_handler},
        {.uri = "/api/rx", .method = HTTP_GET, .handler = receive_handler},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/api/modes", .method = HTTP_GET, .handler = modes_handler},
        {.uri = "/api/config", .method = HTTP_POST, .handler = config_handler},
        {.uri = "/api/image", .method = HTTP_GET, .handler = image_handler},
    };
    for (size_t i = 0; err == ESP_OK && i < sizeof(routes)/sizeof(routes[0]); ++i)
        err = httpd_register_uri_handler(server, &routes[i]);
    if (err != ESP_OK) {
        if (server) httpd_stop(server);
        ESP_LOGE(TAG, "HTTP startup failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "AP %s ready; open http://192.168.4.1 on your phone", CONFIG_PSK_AP_SSID);
    }
    vTaskDelete(NULL);
}

esp_err_t rx_web_start(rx_web_config_cb_t config_cb)
{
    s_config_cb = config_cb;
    snprintf(s_boot_id, sizeof(s_boot_id), "%08" PRIx32 "%08" PRIx32, esp_random(), esp_random());
    return xTaskCreatePinnedToCore(web_task, "rx_ap_start", 6144, NULL, 3, NULL, 1) == pdPASS ?
        ESP_OK : ESP_ERR_NO_MEM;
}
