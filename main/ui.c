#include "ui.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_input.h"
#include "display.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_crc.h"
#include "psk_modes.h"

static const char *TAG = "ui";
static char s_serial_cmd[320];
static size_t s_serial_cmd_len;
static int s_dump_pending;

#define UI_TX_TEXT_DEFAULT \
    "CQ CQ CQ DE HPM6750 PSK MODEM. THIS IS A CONTINUOUS BOARD TO BOARD AUDIO LINK TEST AT 2000 HZ. COPY THE QUICK BROWN FOX 0123456789.\n"

typedef struct {
    const char *label;
    const char *text;
} tx_text_preset_t;

static const tx_text_preset_t s_tx_text_presets[] = {
    {
        "CQ",
        UI_TX_TEXT_DEFAULT,
    },
    {
        "BER",
        "BER TEST FRAME HPM6750: ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789. PACKETS SHOULD REPEAT WITHOUT A NEW SYNC WHILE TX STAYS ACTIVE. END\n",
    },
    {
        "ASCII",
        "ASCII COPY TEST: abcdefghijklmnopqrstuvwxyz ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789 .,:;+-*/=_? HPM6750 PSK AUDIO LINK TEXT SELECTOR\n",
    },
    {
        "LOOP",
        "LONG LOOP MESSAGE: HPM6750 DUAL BOARD PSK TEXT SELECTOR IS RUNNING. AUDIO RATE IS FIXED AT 48 KHZ. MODE CHANGES SEND NEW SYNC; TEXT LOOPS CONTINUOUSLY\n",
    },
};

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
    return *text == '\0' || *text == ' ' || *text == '\t' || *text == '=' || *text == ':';
}

static const char *skip_command_arg(const char *text, const char *keyword)
{
    text += strlen(keyword);
    text = skip_spaces(text);
    if (*text == '=' || *text == ':') {
        text++;
    }
    return skip_spaces(text);
}

static int parse_psk_mode_name(const char *name, psk_mode_t *mode)
{
    for (size_t i = 0; i < psk_mode_count(); ++i) {
        const psk_mode_info_t *info = psk_mode_info_at(i);
        if (info && cmd_equals(name, info->name)) {
            *mode = info->id;
            return 1;
        }
    }
    return 0;
}

static int parse_uint_arg(const char *text, unsigned *value)
{
    if (!text || *text == '\0') {
        return 0;
    }
    unsigned result = 0U;
    for (const char *p = text; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') {
            return 0;
        }
        result = result * 10U + (unsigned)(*p - '0');
    }
    if (value) {
        *value = result;
    }
    return 1;
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

/* Dedicated USB writes preserve binary bytes (stdout applies CR/LF conversion). */
static int capture_usb_write(const void *data, size_t size)
{
    const uint8_t *bytes = data;
    while (size) {
        const int n = usb_serial_jtag_write_bytes(bytes, size > 128 ? 128 : size, pdMS_TO_TICKS(1000));
        if (n <= 0) return 0;
        bytes += n; size -= (size_t)n;
    }
    return 1;
}

static void print_capture_binary(void)
{
    audio_input_capture_stop();
    int16_t samples[256];
    const size_t count = audio_input_capture_count();
    uint32_t crc = 0;
    for (size_t offset = 0; offset < count; offset += 256) {
        const size_t n = audio_input_capture_copy(offset, samples, 256);
        crc = esp_crc32_le(crc, (const uint8_t *)samples, n * sizeof(*samples));
    }
    char header[80];
    const int len = snprintf(header, sizeof(header), "[capbin] samples=%u crc=%08lx\n",
                             (unsigned)count, (unsigned long)crc);
    if (!capture_usb_write(header, len)) return;
    for (size_t offset = 0; offset < count; offset += 256) {
        const size_t n = audio_input_capture_copy(offset, samples, 256);
        if (!capture_usb_write(samples, n * sizeof(*samples))) return;
    }
    capture_usb_write("[capbin-end]\n", 13);
    usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(1000));
}

static int handle_capture_command(const char *command)
{
    const char *arg = skip_spaces(command);
    if (cmd_starts(arg, "CAPBIN")) {
        s_dump_pending = 0;
        print_capture_binary();
        return 1;
    }
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
        printf("[cap] error: request must be 1..1920000 samples and fit available PSRAM\r\n");
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
    if (cmd_starts(cmd, "RXMODE")) {
        cmd = skip_command_arg(cmd, "RXMODE");
    } else if (cmd_starts(cmd, "MODE")) {
        cmd = skip_command_arg(cmd, "MODE");
    }

    psk_mode_t psk_mode;
    if (parse_psk_mode_name(cmd, &psk_mode)) {
        event->type = UI_EVENT_PSK_MODE_CHANGED;
        event->psk_mode = psk_mode;
        return 1;
    }

    return 0;
}

static int parse_serial_control_command(const char *cmd, const display_state_t *display_state,
                                        ui_event_t *event)
{
    while (*cmd == ' ' || *cmd == '\t') {
        cmd++;
    }
    if (*cmd == '@') {
        cmd++;
    }
    cmd = skip_spaces(cmd);

    if (cmd_starts(cmd, "ROLE")) {
        const char *arg = skip_command_arg(cmd, "ROLE");
        if (cmd_equals(arg, "TX")) {
            event->type = UI_EVENT_DISPLAY_ROLE_CHANGED;
            event->display_role = 1;
            return 1;
        }
        if (cmd_equals(arg, "RX")) {
            event->type = UI_EVENT_DISPLAY_ROLE_CHANGED;
            event->display_role = 0;
            return 1;
        }
        printf("[cmd] error role must be TX or RX\r\n");
        return 1;
    }

    if (cmd_starts(cmd, "TXMODE")) {
        const char *arg = skip_command_arg(cmd, "TXMODE");
        psk_mode_t mode;
        if (parse_psk_mode_name(arg, &mode)) {
            event->type = UI_EVENT_TX_MODE_CHANGED;
            event->tx_psk_mode = mode;
            return 1;
        }
        printf("[cmd] error unknown tx mode '%s'\r\n", arg);
        return 1;
    }

    if (cmd_starts(cmd, "TONE") || cmd_starts(cmd, "TXTONE")) {
        const bool tx_tone = cmd_starts(cmd, "TXTONE");
        const char *arg = skip_command_arg(cmd, tx_tone ? "TXTONE" : "TONE");
        char *end = NULL;
        long tone_hz = strtol(arg, &end, 0);
        if (end == arg) {
            printf("[cmd] error tone must be: TONE <100..3500>\r\n");
            return 1;
        }
        if (tone_hz < 100) {
            tone_hz = 100;
        } else if (tone_hz > 3500) {
            tone_hz = 3500;
        }
        event->type = tx_tone ? UI_EVENT_TX_TONE : UI_EVENT_TONE_CHANGED;
        event->tone_hz = (int)tone_hz;
        printf("[tone] selected %dHz\r\n", event->tone_hz);
        return 1;
    }

    if (cmd_starts(cmd, "TXSTOP") || cmd_equals(cmd, "STOP")) {
        event->type = UI_EVENT_TX_STOP;
        return 1;
    }

    if (cmd_starts(cmd, "TXSEL") || cmd_starts(cmd, "TXTEXT")) {
        const char *arg = skip_command_arg(cmd, cmd_starts(cmd, "TXSEL") ? "TXSEL" : "TXTEXT");
        unsigned preset_index = 0U;
        const int has_numeric_index = parse_uint_arg(arg, &preset_index);
        for (size_t i = 0; i < sizeof(s_tx_text_presets) / sizeof(s_tx_text_presets[0]); ++i) {
            if (cmd_equals(arg, s_tx_text_presets[i].label) ||
                (has_numeric_index && preset_index == i)) {
                event->type = UI_EVENT_TX_TEXT;
                event->tx_psk_mode = display_state->selected_tx_psk_mode;
                event->tx_tone_hz = display_state->tx_tone_hz > 0 ? display_state->tx_tone_hz : 2000;
                strncpy(event->tx_text, s_tx_text_presets[i].text, sizeof(event->tx_text) - 1);
                event->tx_text[sizeof(event->tx_text) - 1] = '\0';
                return 1;
            }
        }
        printf("[cmd] error TXSEL must be 0..%u or CQ/BER/ASCII/LOOP\r\n",
               (unsigned)((sizeof(s_tx_text_presets) / sizeof(s_tx_text_presets[0])) - 1U));
        return 1;
    }

    if (cmd_starts(cmd, "TX")) {
        const char *arg = skip_command_arg(cmd, "TX");
        const char *text = *arg ? arg : UI_TX_TEXT_DEFAULT;
        event->type = UI_EVENT_TX_TEXT;
        event->tx_psk_mode = display_state->selected_tx_psk_mode;
        event->tx_tone_hz = display_state->tx_tone_hz > 0 ? display_state->tx_tone_hz : 2000;
        strncpy(event->tx_text, text, sizeof(event->tx_text) - 1);
        event->tx_text[sizeof(event->tx_text) - 1] = '\0';
        return 1;
    }

    if (cmd_starts(cmd, "OUTVOL")) {
        const char *arg = skip_command_arg(cmd, "OUTVOL");
        char *end = NULL;
        const long dac = strtol(arg, &end, 0);
        const long hp = strtol(skip_spaces(end), NULL, 0);
        if (end == arg || dac < 0 || dac > 255 || hp < 0 || hp > 127) {
            printf("[cmd] error output volume must be: OUTVOL <dac 0..255> <hp 0..127>\r\n");
            return 1;
        }
        event->type = UI_EVENT_OUTPUT_GAIN;
        event->output_dac_volume = (int)dac;
        event->output_hp_volume = (int)hp;
        return 1;
    }

    if (cmd_starts(cmd, "INGAIN") || cmd_starts(cmd, "MICGAIN")) {
        const char *arg = skip_command_arg(cmd, cmd_starts(cmd, "INGAIN") ? "INGAIN" : "MICGAIN");
        char *end = NULL;
        const float gain_db = strtof(arg, &end);
        if (end == arg || gain_db < 0.0f || gain_db > 42.0f) {
            printf("[cmd] error input gain must be: INGAIN <0..42 dB>\r\n");
            return 1;
        }
        event->type = UI_EVENT_INPUT_GAIN;
        event->input_gain_db = gain_db;
        printf("[gain] input %.1fdB\r\n", (double)gain_db);
        return 1;
    }

    if (cmd_starts(cmd, "RXDIAG") || cmd_starts(cmd, "DIAG")) {
        event->type = UI_EVENT_RX_DIAG;
        return 1;
    }

    if (cmd_starts(cmd, "FECBRANCH")) {
        const char *arg = skip_spaces(skip_command_arg(cmd, "FECBRANCH"));
        if (cmd_equals(arg, "AUTO")) {
            event->type = UI_EVENT_RX_FEC_BRANCH;
            event->fec_branch_override = -1;
            printf("[fecbranch] auto\r\n");
            return 1;
        }
        char *end = NULL;
        const long branch = strtol(arg, &end, 0);
        if (end == arg || branch < 0 || branch > 1) {
            printf("[cmd] error FECBRANCH must be AUTO, 0, or 1\r\n");
            return 1;
        }
        event->type = UI_EVENT_RX_FEC_BRANCH;
        event->fec_branch_override = (int)branch;
        printf("[fecbranch] %d\r\n", event->fec_branch_override);
        return 1;
    }

    if (cmd_starts(cmd, "CONTENT")) {
        const char *arg = skip_command_arg(cmd, "CONTENT");
        const char *names[] = {"TEXT", "IMAGE", "AUDIO"};
        for (int i = 0; i < CONTENT_COUNT; ++i) {
            if (cmd_equals(arg, names[i])) {
                event->type = UI_EVENT_CONTENT_CHANGED; event->content_type = i; return 1;
            }
        }
        printf("[cmd] CONTENT TEXT|IMAGE|AUDIO\r\n"); return 1;
    }
    if (cmd_starts(cmd, "APRATE") || cmd_starts(cmd, "AP580RATE")) {
        const char *key = cmd_starts(cmd, "APRATE") ? "APRATE" : "AP580RATE";
        unsigned rate = 0;
        if (parse_uint_arg(skip_command_arg(cmd, key), &rate) &&
            (rate == 600 || rate == 1200 || rate == 2400)) {
            event->type = UI_EVENT_VOICE_RATE; event->codec_rate = rate; return 1;
        }
        printf("[cmd] APRATE 600|1200|2400; match AP580 jumpers\r\n"); return 1;
    }

    if (cmd_starts(cmd, "STATUS")) {
        printf("[status] role=%s rxmode=%s psk=%s txmode=%s tone=%dHz txtone=%dHz tx=%d/%u content=%s rate=%lu\r\n",
               display_state->display_role ? "TX" : "RX",
               modem_mode_name(display_state->selected_mode),
               psk_mode_name(display_state->selected_psk_mode),
               psk_mode_name(display_state->selected_tx_psk_mode),
               display_state->rx_tone_hz,
               display_state->tx_tone_hz,
               display_state->tx_active,
               (unsigned)display_state->tx_queued, content_name(display_state->content_type),
               (unsigned long)display_state->codec_rate);
        return 1;
    }

    return 0;
}

static ui_event_t poll_serial_command(const display_state_t *display_state)
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
                    const char *cmd = s_serial_cmd[0] == '@' ? s_serial_cmd + 1 : s_serial_cmd;
                    if (handle_capture_command(cmd)) {
                        ESP_LOGI(TAG, "serial capture command: %s", s_serial_cmd);
                    } else if (parse_serial_control_command(s_serial_cmd, display_state, &event)) {
                        ESP_LOGI(TAG, "serial control command: %s", s_serial_cmd);
                    } else if (parse_serial_mode_command(s_serial_cmd, &event)) {
                        ESP_LOGI(TAG, "serial mode command: %s", s_serial_cmd);
                    } else {
                        ESP_LOGW(TAG, "unknown serial command: %s", s_serial_cmd);
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

    ESP_LOGI(TAG, "LCD and USB controls ready; no GPIO button input");
    return ESP_OK;
}

ui_event_t ui_poll(const display_state_t *display_state)
{
    ui_event_t event = { .type = UI_EVENT_NONE };
    finish_pending_capture_dump();

    event = poll_serial_command(display_state);
    if (event.type != UI_EVENT_NONE) {
        return event;
    }

    int display_role;
    if (display_take_role_selection(&display_role)) {
        event.type = UI_EVENT_DISPLAY_ROLE_CHANGED;
        event.display_role = display_role != 0;
        return event;
    }

    content_type_t content;
    if (display_take_content_selection(&content)) {
        event.type = UI_EVENT_CONTENT_CHANGED; event.content_type = content; return event;
    }

    psk_mode_t display_psk_mode;
    if (display_take_mode_selection(&display_psk_mode)) {
        if (display_state->display_role) {
            event.type = UI_EVENT_TX_MODE_CHANGED;
            event.tx_psk_mode = display_psk_mode;
        } else {
            event.type = UI_EVENT_PSK_MODE_CHANGED;
            event.psk_mode = display_psk_mode;
        }
        return event;
    }

    int tone_hz;
    if (display_take_tone_selection(&tone_hz)) {
        event.type = display_state->display_role ? UI_EVENT_TX_TONE : UI_EVENT_TONE_CHANGED;
        event.tone_hz = tone_hz;
        return event;
    }

    return event;
}
