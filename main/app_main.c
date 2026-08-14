#include <ctype.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio_input.h"
#include "board_config.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psk_modem.h"

static const char *TAG = "psk-modem";

#define RX_LOG_FLUSH_BYTES 160U
#define RX_LOG_FLUSH_US 250000LL

static psk_modem_t s_modem;
static char s_rx_log[256];
static size_t s_rx_log_len;
static int64_t s_last_rx_log_us;
static uint8_t s_rx_log_hex_mode;

static void flush_rx_log(int64_t now_us);

static uint32_t block_abs_avg(const int16_t *samples, size_t count)
{
    uint64_t sum = 0;
    for (size_t i = 0; i < count; ++i) {
        const int32_t s = samples[i];
        sum += (uint32_t)(s < 0 ? -s : s);
    }
    return count ? (uint32_t)(sum / count) : 0;
}

static int ascii_equal_ignore_case(const char *a, const char *b)
{
    while (*a && *b) {
        if (toupper((unsigned char)*a++) != toupper((unsigned char)*b++)) {
            return 0;
        }
    }
    return *a == '\0' && *b == '\0';
}

static int ascii_starts_ignore_case(const char *text, const char *prefix)
{
    while (*prefix) {
        if (toupper((unsigned char)*text++) != toupper((unsigned char)*prefix++)) {
            return 0;
        }
    }
    return *text == '\0' || *text == ' ' || *text == '\t' || *text == '=' || *text == ':';
}

static char *trim_ascii(char *s)
{
    while (*s == ' ' || *s == '\t') {
        ++s;
    }
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t')) {
        *--end = '\0';
    }
    return s;
}

static int find_mode_by_name(const char *name, psk_mode_t *mode)
{
    for (size_t i = 0; i < psk_mode_count(); ++i) {
        const psk_mode_info_t *info = psk_mode_info_at(i);
        if (info && ascii_equal_ignore_case(name, info->name)) {
            *mode = info->id;
            return 1;
        }
    }
    return 0;
}

static void print_rx_hex(const char *text, size_t len)
{
    printf("[rxhex]");
    for (size_t i = 0; i < len; ++i) {
        printf(" %02X", (unsigned char)text[i]);
    }
    printf("\n");
}

static void print_rx_text(const char *text, size_t len)
{
    printf("[rx] ");
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = (unsigned char)text[i];
        if (c >= 32U && c <= 126U) {
            putchar(c);
        } else if (c == '\r') {
            printf("\\r");
        } else if (c == '\n') {
            printf("\\n");
        } else {
            printf("\\x%02X", c);
        }
    }
    printf("\n");
}

static void append_rx_log(const char *text, size_t len, int64_t now_us)
{
    while (len > 0U) {
        const size_t room = sizeof(s_rx_log) - s_rx_log_len;
        if (room == 0U) {
            flush_rx_log(now_us);
            continue;
        }
        const size_t n = len < room ? len : room;
        memcpy(&s_rx_log[s_rx_log_len], text, n);
        s_rx_log_len += n;
        text += n;
        len -= n;

        if (s_rx_log_len >= RX_LOG_FLUSH_BYTES || s_rx_log_len == sizeof(s_rx_log)) {
            flush_rx_log(now_us);
        }
    }

    if (s_rx_log_len > 0U && now_us - s_last_rx_log_us >= RX_LOG_FLUSH_US) {
        flush_rx_log(now_us);
    }
}

static void flush_rx_log(int64_t now_us)
{
    if (s_rx_log_len == 0U) {
        return;
    }
    if (s_rx_log_hex_mode) {
        print_rx_hex(s_rx_log, s_rx_log_len);
    } else {
        print_rx_text(s_rx_log, s_rx_log_len);
    }
    s_rx_log_len = 0U;
    s_last_rx_log_us = now_us;
}

static void print_status(void)
{
    psk_modem_debug_t debug;
    psk_modem_debug_snapshot(&s_modem, &debug);
    ESP_LOGI(TAG,
             "mode=%s tone=%dHz selected=%dHz afc=%d sql=%d/%d open=%d chars=%lu symbols=%lu log=%s",
             psk_modem_mode_name(psk_modem_mode(&s_modem)),
             debug.tone_hz,
             debug.selected_tone_hz,
             debug.afc_enabled,
             debug.sq_level,
             debug.sq_thresh,
             debug.sq_open,
             (unsigned long)debug.chars,
             (unsigned long)debug.symbols,
             s_rx_log_hex_mode ? "HEX" : "TEXT");
}

static void handle_command(char *cmd)
{
    cmd = trim_ascii(cmd);
    if (*cmd == '\0') {
        return;
    }

    if (ascii_starts_ignore_case(cmd, "MODE")) {
        char *arg = trim_ascii(cmd + 4);
        if (*arg == '=' || *arg == ':') {
            arg = trim_ascii(arg + 1);
        }
        psk_mode_t mode;
        if (find_mode_by_name(arg, &mode)) {
            flush_rx_log(esp_timer_get_time());
            psk_modem_set_mode(&s_modem, mode);
            ESP_LOGI(TAG, "mode selected: %s", psk_modem_mode_name(mode));
        } else {
            ESP_LOGW(TAG, "unknown mode: %s", arg);
        }
    } else if (ascii_starts_ignore_case(cmd, "TONE")) {
        const int tone_hz = atoi(trim_ascii(cmd + 4));
        psk_modem_set_tone(&s_modem, tone_hz);
        ESP_LOGI(TAG, "tone selected: %d Hz", psk_modem_tone_hz(&s_modem));
    } else if (ascii_starts_ignore_case(cmd, "AFC")) {
        char *arg = trim_ascii(cmd + 3);
        const int enabled = (*arg == '\0') ? 1 : (atoi(arg) != 0);
        psk_modem_set_afc(&s_modem, enabled);
        ESP_LOGI(TAG, "afc: %s", enabled ? "on" : "off");
    } else if (ascii_starts_ignore_case(cmd, "SQL")) {
        char *arg = trim_ascii(cmd + 3);
        const int enabled = (*arg == '\0') ? 1 : (atoi(arg) != 0);
        psk_modem_set_squelch_enabled(&s_modem, enabled);
        ESP_LOGI(TAG, "squelch: %s", enabled ? "on" : "off");
    } else if (ascii_starts_ignore_case(cmd, "LOG")) {
        char *arg = trim_ascii(cmd + 3);
        if (*arg == '=' || *arg == ':') {
            arg = trim_ascii(arg + 1);
        }
        if (ascii_equal_ignore_case(arg, "HEX")) {
            s_rx_log_hex_mode = 1;
        } else if (ascii_equal_ignore_case(arg, "TEXT")) {
            s_rx_log_hex_mode = 0;
        } else {
            ESP_LOGW(TAG, "unknown log mode: %s", arg);
            return;
        }
        ESP_LOGI(TAG, "rx log format: %s", s_rx_log_hex_mode ? "HEX" : "TEXT");
    } else if (ascii_equal_ignore_case(cmd, "STATUS")) {
        print_status();
    } else if (ascii_equal_ignore_case(cmd, "HELP")) {
        printf("Commands: MODE <name>, TONE <hz>, AFC [0|1], SQL [0|1], LOG TEXT|HEX, STATUS\n");
    } else {
        ESP_LOGW(TAG, "unknown command: %s", cmd);
    }
}

static void poll_serial_commands(void)
{
    static char line[80];
    static size_t used;

    for (unsigned drained = 0; drained < 64U; ++drained) {
        const int ch = getchar();
        if (ch == EOF) {
            break;
        }
        if (ch == '\r' || ch == '\n') {
            if (used > 0U) {
                line[used] = '\0';
                used = 0U;
                handle_command(line);
            }
        } else if ((ch == '\b' || ch == 0x7F) && used > 0U) {
            --used;
        } else if (ch >= 32 && ch <= 126 && used + 1U < sizeof(line)) {
            line[used++] = (char)ch;
        } else if (used + 1U >= sizeof(line)) {
            used = 0U;
            ESP_LOGW(TAG, "command line too long");
        }
    }
}

void app_main(void)
{
    (void)fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);

    const psk_modem_config_t cfg = {
        .mode = PSK_MODE_BPSK31,
        .sample_rate_hz = BOARD_MODEM_SAMPLE_RATE_HZ,
        .rx_tone_hz = BOARD_DEFAULT_RX_TONE_HZ,
        .squelch = BOARD_DEFAULT_SQUELCH,
        .afc_enabled = 1,
        .squelch_enabled = 1,
    };
    psk_modem_init(&s_modem, &cfg);

    ESP_ERROR_CHECK(audio_input_init());
    ESP_LOGI(TAG, "%s PSK modem started; type HELP for commands", BOARD_NAME);
    ESP_LOGI(TAG, "%lu PSK modes available; default %s at %d Hz",
             (unsigned long)psk_mode_count(),
             psk_modem_mode_name(psk_modem_mode(&s_modem)),
             BOARD_DEFAULT_RX_TONE_HZ);

    int16_t samples[BOARD_AUDIO_BLOCK_SAMPLES];
    char decoded[128];
    int64_t last_diag_us = 0;

    while (true) {
        poll_serial_commands();

        const size_t count = audio_input_read(samples, BOARD_AUDIO_BLOCK_SAMPLES);
        const int64_t now_us = esp_timer_get_time();
        if (count > 0U) {
            const uint32_t avg = block_abs_avg(samples, count);
            if (avg >= BOARD_AUDIO_ACTIVE_ABS_AVG_MIN) {
                const size_t got = psk_modem_process(&s_modem, samples, count, decoded, sizeof(decoded));
                if (got > 0U) {
                    append_rx_log(decoded, got, now_us);
                }
            } else {
                flush_rx_log(now_us);
            }
        }

#if BOARD_MODEM_DEBUG_LOGS
        if (now_us - last_diag_us >= 2000000LL) {
            last_diag_us = now_us;
            const uint32_t avg = block_abs_avg(samples, count);
            psk_modem_debug_t debug;
            psk_modem_debug_snapshot(&s_modem, &debug);
            ESP_LOGI(TAG,
                     "audio_abs=%lu mode=%s tone=%dHz sq=%d/%d open=%d chars=%lu symbols=%lu",
                     (unsigned long)avg,
                     psk_modem_mode_name(psk_modem_mode(&s_modem)),
                     debug.tone_hz,
                     debug.sq_level,
                     debug.sq_thresh,
                     debug.sq_open,
                     (unsigned long)debug.chars,
                     (unsigned long)debug.symbols);
        }
#endif

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
