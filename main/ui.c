#include "ui.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

#include "audio_input.h"
#include "board_config.h"
#include "display.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "psk_modes.h"

static const char *TAG = "ui";
static int s_last_sw1 = 1;
static char s_serial_cmd[32];
static size_t s_serial_cmd_len;
static int s_dump_pending;

typedef enum {
    UI_CYCLE_KIND_PSK,
    UI_CYCLE_KIND_MODE,
} ui_cycle_kind_t;

typedef struct {
    ui_cycle_kind_t kind;
    union {
        psk_mode_t psk_mode;
        modem_mode_t modem_mode;
    } value;
} ui_cycle_item_t;

static const ui_cycle_item_t s_mode_cycle[] = {
    { UI_CYCLE_KIND_PSK,  { .psk_mode = PSK_MODE_BPSK31 } },
    { UI_CYCLE_KIND_PSK,  { .psk_mode = PSK_MODE_BPSK63 } },
    { UI_CYCLE_KIND_PSK,  { .psk_mode = PSK_MODE_BPSK63F } },
    { UI_CYCLE_KIND_PSK,  { .psk_mode = PSK_MODE_BPSK125 } },
    { UI_CYCLE_KIND_PSK,  { .psk_mode = PSK_MODE_BPSK250 } },
    { UI_CYCLE_KIND_PSK,  { .psk_mode = PSK_MODE_BPSK500 } },
    { UI_CYCLE_KIND_PSK,  { .psk_mode = PSK_MODE_BPSK1000 } },
    { UI_CYCLE_KIND_PSK,  { .psk_mode = PSK_MODE_12XPSK125 } },
    { UI_CYCLE_KIND_PSK,  { .psk_mode = PSK_MODE_6XPSK250 } },
    { UI_CYCLE_KIND_PSK,  { .psk_mode = PSK_MODE_2XPSK500 } },
    { UI_CYCLE_KIND_PSK,  { .psk_mode = PSK_MODE_4XPSK500 } },
    { UI_CYCLE_KIND_PSK,  { .psk_mode = PSK_MODE_2XPSK800 } },
    { UI_CYCLE_KIND_PSK,  { .psk_mode = PSK_MODE_2XPSK1000 } },
    { UI_CYCLE_KIND_PSK,  { .psk_mode = PSK_MODE_QPSK31 } },
    { UI_CYCLE_KIND_PSK,  { .psk_mode = PSK_MODE_QPSK63 } },
    { UI_CYCLE_KIND_PSK,  { .psk_mode = PSK_MODE_QPSK125 } },
    { UI_CYCLE_KIND_PSK,  { .psk_mode = PSK_MODE_QPSK250 } },
    { UI_CYCLE_KIND_PSK,  { .psk_mode = PSK_MODE_QPSK500 } },
    { UI_CYCLE_KIND_PSK,  { .psk_mode = PSK_MODE_8PSK125 } },
    { UI_CYCLE_KIND_PSK,  { .psk_mode = PSK_MODE_8PSK250 } },
    { UI_CYCLE_KIND_PSK,  { .psk_mode = PSK_MODE_8PSK500 } },
    { UI_CYCLE_KIND_PSK,  { .psk_mode = PSK_MODE_8PSK1000 } },
    { UI_CYCLE_KIND_PSK,  { .psk_mode = PSK_MODE_8PSK125F } },
    { UI_CYCLE_KIND_PSK,  { .psk_mode = PSK_MODE_8PSK250F } },
    { UI_CYCLE_KIND_PSK,  { .psk_mode = PSK_MODE_8PSK500F } },
    { UI_CYCLE_KIND_PSK,  { .psk_mode = PSK_MODE_8PSK1000F } },
    { UI_CYCLE_KIND_PSK,  { .psk_mode = PSK_MODE_8PSK1200F } },
    { UI_CYCLE_KIND_MODE, { .modem_mode = MODEM_MODE_FT8 } },
    { UI_CYCLE_KIND_MODE, { .modem_mode = MODEM_MODE_FT4 } },
    { UI_CYCLE_KIND_MODE, { .modem_mode = MODEM_MODE_RTTY } },
    { UI_CYCLE_KIND_MODE, { .modem_mode = MODEM_MODE_CW } },
    { UI_CYCLE_KIND_MODE, { .modem_mode = MODEM_MODE_MFSK16 } },
};

static int cycle_item_matches(const display_state_t *state, const ui_cycle_item_t *item)
{
    if (item->kind == UI_CYCLE_KIND_MODE) {
        return state->selected_mode == item->value.modem_mode;
    }
    if (item->value.psk_mode == PSK_MODE_BPSK31) {
        return state->selected_mode == MODEM_MODE_BPSK31 && state->selected_psk_mode == PSK_MODE_BPSK31;
    }
    return state->selected_mode == MODEM_MODE_PSK63 && state->selected_psk_mode == item->value.psk_mode;
}

static ui_event_t cycle_next_mode(const display_state_t *state)
{
    const size_t count = sizeof(s_mode_cycle) / sizeof(s_mode_cycle[0]);
    size_t current = 0;
    for (size_t i = 0; i < count; ++i) {
        if (cycle_item_matches(state, &s_mode_cycle[i])) {
            current = i;
            break;
        }
    }

    const ui_cycle_item_t *next = &s_mode_cycle[(current + 1U) % count];
    ui_event_t event = { .type = next->kind == UI_CYCLE_KIND_PSK ? UI_EVENT_PSK_MODE_CHANGED : UI_EVENT_MODE_CHANGED };
    if (next->kind == UI_CYCLE_KIND_PSK) {
        event.psk_mode = next->value.psk_mode;
    } else {
        event.mode = next->value.modem_mode;
    }
    return event;
}

static int cmd_equals(const char *a, const char *b)
{
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static const char *skip_spaces(const char *text)
{
    while (*text == ' ' || *text == '\t') {
        text++;
    }
    return text;
}

static int cmd_starts(const char *text, const char *keyword)
{
    while (*keyword) {
        if (toupper((unsigned char)*text) != toupper((unsigned char)*keyword)) {
            return 0;
        }
        text++;
        keyword++;
    }
    return *text == '\0' || *text == ' ' || *text == '\t';
}

static void print_capture_dump(void)
{
    int16_t samples[64];
    const size_t count = audio_input_capture_count();

    printf("[capdump] samples=%u\r\n", (unsigned)count);
    for (size_t offset = 0; offset < count; offset += sizeof(samples) / sizeof(samples[0])) {
        const size_t copied = audio_input_capture_copy(offset, samples,
                                                       sizeof(samples) / sizeof(samples[0]));
        printf("[cap] %05u:", (unsigned)offset);
        for (size_t i = 0; i < copied; ++i) {
            printf(" %04x", (unsigned)(uint16_t)samples[i]);
        }
        printf("\r\n");
    }
}

static int handle_capture_command(const char *command)
{
    const char *arg = skip_spaces(command);
    if (cmd_starts(arg, "CAPDUMP")) {
        s_dump_pending = 0;
        print_capture_dump();
        return 1;
    }

    const int is_dump = cmd_starts(arg, "DUMP");
    if (!is_dump && !cmd_starts(arg, "CAP")) {
        return 0;
    }
    arg += is_dump ? 4 : 3;
    arg = skip_spaces(arg);
    const unsigned requested = *arg ? (unsigned)strtoul(arg, NULL, 10) :
                                      (is_dump ? 1U : 32768U);
    const size_t max_samples = is_dump ? (size_t)requested * 256U : (size_t)requested;
    if (audio_input_capture_start(max_samples) != ESP_OK) {
        printf("[cap] error samples must be 1..32768\r\n");
        return 1;
    }
    s_dump_pending = is_dump;
    printf("[cap] armed=%u%s\r\n", (unsigned)max_samples, is_dump ? " dump=1" : "");
    return 1;
}

static void finish_pending_capture_dump(void)
{
    if (s_dump_pending && !audio_input_capture_active()) {
        s_dump_pending = 0;
        print_capture_dump();
    }
}

static int parse_serial_mode_command(const char *cmd, ui_event_t *event)
{
    while (*cmd == ' ' || *cmd == '\t') {
        cmd++;
    }
    if (*cmd == '@') {
        cmd++;
    }
    while (*cmd == ' ' || *cmd == '\t') {
        cmd++;
    }
    if ((toupper((unsigned char)cmd[0]) == 'M') &&
        (toupper((unsigned char)cmd[1]) == 'O') &&
        (toupper((unsigned char)cmd[2]) == 'D') &&
        (toupper((unsigned char)cmd[3]) == 'E') &&
        (cmd[4] == ' ' || cmd[4] == '\t')) {
        cmd += 5;
        while (*cmd == ' ' || *cmd == '\t') {
            cmd++;
        }
    }

    for (size_t i = 0; i < psk_mode_count(); ++i) {
        const psk_mode_info_t *info = psk_mode_info_at(i);
        if (!info) {
            continue;
        }
        if (cmd_equals(cmd, info->name)) {
            event->type = UI_EVENT_PSK_MODE_CHANGED;
            event->psk_mode = info->id;
            return 1;
        }
    }

    if (cmd_equals(cmd, "FT8")) {
        event->type = UI_EVENT_MODE_CHANGED;
        event->mode = MODEM_MODE_FT8;
        return 1;
    }
    if (cmd_equals(cmd, "FT4")) {
        event->type = UI_EVENT_MODE_CHANGED;
        event->mode = MODEM_MODE_FT4;
        return 1;
    }
    if (cmd_equals(cmd, "RTTY")) {
        event->type = UI_EVENT_MODE_CHANGED;
        event->mode = MODEM_MODE_RTTY;
        return 1;
    }
    if (cmd_equals(cmd, "CW")) {
        event->type = UI_EVENT_MODE_CHANGED;
        event->mode = MODEM_MODE_CW;
        return 1;
    }
    if (cmd_equals(cmd, "MFSK16")) {
        event->type = UI_EVENT_MODE_CHANGED;
        event->mode = MODEM_MODE_MFSK16;
        return 1;
    }

    return 0;
}

static ui_event_t poll_serial_command(void)
{
    ui_event_t event = { .type = UI_EVENT_NONE };
    uint8_t data[32];
    int len;

    do {
        len = usb_serial_jtag_read_bytes(data, sizeof(data), 0);
        if (len <= 0) {
            break;
        }
        for (int i = 0; i < len; ++i) {
            const int ch = data[i];
            if (ch == '\r' || ch == '\n') {
                if (s_serial_cmd_len > 0) {
                    s_serial_cmd[s_serial_cmd_len] = '\0';
                    if (s_serial_cmd[0] == '@') {
                        if (handle_capture_command(s_serial_cmd + 1)) {
                            ESP_LOGI(TAG, "serial capture command: %s", s_serial_cmd);
                        } else if (parse_serial_mode_command(s_serial_cmd, &event)) {
                            ESP_LOGI(TAG, "serial mode command: %s", s_serial_cmd);
                        } else {
                            ESP_LOGW(TAG, "unknown serial mode command: %s", s_serial_cmd);
                        }
                    }
                    s_serial_cmd_len = 0;
                    if (event.type != UI_EVENT_NONE) {
                        return event;
                    }
                }
            } else if (s_serial_cmd_len + 1U < sizeof(s_serial_cmd)) {
                s_serial_cmd[s_serial_cmd_len++] = (char)ch;
            } else {
                s_serial_cmd_len = 0;
            }
        }
    } while (len > 0);

    return event;
}

esp_err_t ui_init(void)
{
    usb_serial_jtag_driver_config_t usb_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t usb_err = usb_serial_jtag_driver_install(&usb_cfg);
    if (usb_err != ESP_OK) {
        ESP_LOGW(TAG, "USB serial/JTAG RX driver not installed: %s", esp_err_to_name(usb_err));
    }

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_SW1_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    ESP_LOGI(TAG, "SW1 on GPIO%d; touch backend placeholder", BOARD_SW1_GPIO);
    return ESP_OK;
}

ui_event_t ui_poll(const display_state_t *display_state)
{
    ui_event_t event = { .type = UI_EVENT_NONE };
    display_task_handler();
    finish_pending_capture_dump();

    event = poll_serial_command();
    if (event.type != UI_EVENT_NONE) {
        return event;
    }

    int tone_hz;
    if (display_take_tone_selection(&tone_hz)) {
        event.type = UI_EVENT_TONE_CHANGED;
        event.tone_hz = tone_hz;
        return event;
    }

    const int sw1 = gpio_get_level(BOARD_SW1_GPIO);
    if (s_last_sw1 == 1 && sw1 == 0) {
        s_last_sw1 = sw1;
        return cycle_next_mode(display_state);
    }
    s_last_sw1 = sw1;

    return event;
}
