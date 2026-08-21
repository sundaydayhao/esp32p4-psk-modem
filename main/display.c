#include "display.h"

#include <stdint.h>
#include <stdio.h>
#include <limits.h>
#include <string.h>

#include "board_config.h"
#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "rx_modem.h"

#define MODE_COLUMNS 3U
#define MODE_ROWS 9U
#define LEFT_PANEL_X 20U
#define LEFT_PANEL_Y 56U
#define MODE_PANEL_X 520U
#define MODE_PANEL_Y 56U
#define ROLE_BUTTON_X 12U
#define ROLE_BUTTON_Y 10U
#define ROLE_BUTTON_STEP_X 116U
#define ROLE_BUTTON_W 108U
#define ROLE_BUTTON_H 32U
#define MODE_BUTTON_X 8U
#define MODE_BUTTON_Y 84U
#define MODE_BUTTON_STEP_X 80U
#define MODE_BUTTON_STEP_Y 36U
#define MODE_BUTTON_W 76U
#define MODE_BUTTON_H 31U
#define WATERFALL_X 14U
#define WATERFALL_Y 76U
#define WATERFALL_WIDTH 456U
#define WATERFALL_HEIGHT 150U
#define WATERFALL_COLOR_BITS 16U
#define WATERFALL_MAX_HZ 3000U
#define WATERFALL_FREQ_LABEL_WIDTH 48U
#define WATERFALL_LABEL_Y 229U
#define TEXT_PANEL_X 14U
#define TEXT_PANEL_Y 248U
#define TX_TEXT_BUTTON_COUNT 4U
#define TX_TEXT_BUTTON_X 96U
#define TX_TEXT_BUTTON_Y 7U
#define TX_TEXT_BUTTON_STEP_X 88U
#define TX_TEXT_BUTTON_W 82U
#define TX_TEXT_BUTTON_H 25U
#define TEXT_DISPLAY_TAIL_CHARS 520U
#define TEXT_DISPLAY_CACHE_LEN (TEXT_DISPLAY_TAIL_CHARS + 1U)

static uint8_t g_lvgl_ready;
static int32_t g_screen_w = 800;
static int32_t g_screen_h = 480;
static int32_t g_left_panel_x;
static int32_t g_left_panel_y;
static int32_t g_left_panel_w;
static int32_t g_left_panel_h;
static int32_t g_right_panel_x;
static int32_t g_right_panel_y;
static int32_t g_right_panel_w;
static int32_t g_right_panel_h;
static int32_t g_header_h;
static int32_t g_waterfall_x;
static int32_t g_waterfall_y;
static int32_t g_waterfall_w;
static int32_t g_waterfall_h;
static int32_t g_text_panel_x;
static int32_t g_text_panel_y;
static int32_t g_text_panel_w;
static int32_t g_text_panel_h;
static int32_t g_role_button_x;
static int32_t g_role_button_y;
static int32_t g_role_button_step_x;
static int32_t g_role_button_w;
static int32_t g_role_button_h;
static int32_t g_mode_button_x;
static int32_t g_mode_button_y;
static int32_t g_mode_button_step_x;
static int32_t g_mode_button_step_y;
static int32_t g_mode_button_w;
static int32_t g_mode_button_h;
static int32_t g_tx_text_button_x;
static int32_t g_tx_text_button_y;
static int32_t g_tx_text_button_step_x;
static int32_t g_tx_text_button_w;
static int32_t g_tx_text_button_h;
static lv_obj_t *g_mode_label;
static lv_obj_t *g_baud_label;
static lv_obj_t *g_tone_label;
static lv_obj_t *g_input_label;
static lv_obj_t *g_touch_label;
static lv_obj_t *g_text_view;
static lv_obj_t *g_rx_label;
static lv_obj_t *g_text_title_label;
static lv_obj_t *g_mode_panel_title_label;
static lv_obj_t *g_waterfall_canvas;
static lv_obj_t *g_role_buttons[2];
static lv_obj_t *g_mode_buttons[32];
static lv_obj_t *g_tx_text_buttons[TX_TEXT_BUTTON_COUNT];
static display_mode_selected_cb_t g_mode_cb;
static void *g_mode_cb_user;
static display_role_selected_cb_t g_role_cb;
static void *g_role_cb_user;
static display_tx_text_selected_cb_t g_tx_text_cb;
static void *g_tx_text_cb_user;
static display_role_t g_current_role = DISPLAY_ROLE_RX;
static uint8_t g_tx_text_selection;
static char g_text_cache[TEXT_DISPLAY_CACHE_LEN];
static uint8_t g_text_cache_valid;
static display_role_t g_text_layout_role;
static int g_pending_tone_hz;
static int g_has_pending_tone;
static int g_band_min_hz = 100;
static int g_band_max_hz = 3500;
static uint8_t *g_waterfall_buffer;
static size_t g_waterfall_buffer_len;
static uint32_t g_waterfall_stride;
static display_role_t g_last_role = (display_role_t)0xFF;
static modem_mode_t g_last_mode = (modem_mode_t)0xFF;
static psk_mode_t g_last_psk_mode = (psk_mode_t)0xFF;
static int g_last_tone_hz = INT_MIN;
static int g_last_peak_hz = INT_MIN;
static uint32_t g_last_tx_queued = UINT32_MAX;
static uint32_t g_last_tx_active = UINT32_MAX;
static uint16_t g_last_input_abs_avg = UINT16_MAX;
static uint8_t g_last_selected_lane = UINT8_MAX;
static uint8_t g_last_tx_text_selection = UINT8_MAX;

static void build_ui(void);
static lv_obj_t *make_panel(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h);
static lv_obj_t *make_label(lv_obj_t *parent, const char *text, int32_t x, int32_t y, int32_t w,
                            const lv_font_t *font, lv_color_t color);
static void role_button_event_cb(lv_event_t *event);
static void mode_button_event_cb(lv_event_t *event);
static void tx_text_button_event_cb(lv_event_t *event);
static void spectrum_touch_cb(lv_event_t *event);
static void update_role_buttons(display_role_t role);
static void update_mode_buttons(const psk_mode_info_t *active_mode);
static void update_tx_text_buttons(display_role_t role);
static void update_touch_label(void);
static void format_baud(char *buf, size_t len, const psk_mode_info_t *mode);
static const char *tail_text(const char *text, size_t max_len);
static void scroll_text_to_bottom(void);
static void waterfall_push(const spectrum_t *spectrum);
static uint16_t waterfall_color(uint8_t level);
static void make_waterfall_frequency_labels(lv_obj_t *parent);
static const char *mode_display_name(modem_mode_t mode, psk_mode_t psk_mode);
static int32_t scale_x(int32_t value);
static int32_t scale_y(int32_t value);
static void update_layout_metrics(void);

esp_err_t display_init(void)
{
    printf("[display] lvgl init begin\r\n");
    lv_display_t *disp = bsp_display_start();
    if (!disp) {
        return ESP_FAIL;
    }

    g_screen_w = lv_display_get_horizontal_resolution(disp);
    g_screen_h = lv_display_get_vertical_resolution(disp);
    if (g_screen_w <= 0) {
        g_screen_w = 800;
    }
    if (g_screen_h <= 0) {
        g_screen_h = 480;
    }
    update_layout_metrics();

    build_ui();
    g_lvgl_ready = 1U;
    bsp_display_backlight_on();
    printf("[display] lvgl backend ready\r\n");
    return ESP_OK;
}

void display_task(void)
{
}

void display_task_handler(void)
{
}

void display_set_mode_selected_callback(display_mode_selected_cb_t cb, void *user)
{
    g_mode_cb = cb;
    g_mode_cb_user = user;
}

void display_set_role_selected_callback(display_role_selected_cb_t cb, void *user)
{
    g_role_cb = cb;
    g_role_cb_user = user;
}

void display_set_tx_text_selected_callback(display_tx_text_selected_cb_t cb, void *user)
{
    g_tx_text_cb = cb;
    g_tx_text_cb_user = user;
}

void display_set_tx_text_selection(uint8_t index)
{
    if (index >= TX_TEXT_BUTTON_COUNT) {
        return;
    }
    g_tx_text_selection = index;
}

void display_status(const char *line1, const char *line2)
{
    if (!g_lvgl_ready) {
        printf("[status] %s | %s\r\n", line1 ? line1 : "", line2 ? line2 : "");
        return;
    }

    if (!bsp_display_lock(0)) {
        return;
    }
    lv_label_set_text_fmt(g_mode_label, "%s", line1 ? line1 : "");
    lv_label_set_text_fmt(g_baud_label, "%s", line2 ? line2 : "");
    bsp_display_unlock();
}

void display_append_text(display_state_t *state, const char *text)
{
    if (!state || !text) {
        return;
    }

    const size_t add = strlen(text);
    if (add == 0U) {
        return;
    }
    if (add >= sizeof(state->text)) {
        text += add - sizeof(state->text) + 1U;
    }

    size_t keep = state->text_len;
    const size_t incoming = strlen(text);
    if (keep + incoming >= sizeof(state->text)) {
        const size_t drop = keep + incoming - sizeof(state->text) + 1U;
        memmove(state->text, state->text + drop, keep - drop);
        keep -= drop;
    }
    memcpy(state->text + keep, text, incoming + 1U);
    state->text_len = keep + incoming;
}

void display_render(const display_state_t *state, const spectrum_t *spectrum)
{
    if (!state) {
        return;
    }

    const int peak_hz = spectrum ? spectrum->peak_hz : 0;
    const int is_tx = state->display_role == DISPLAY_ROLE_TX;
    g_current_role = state->display_role;
    g_band_min_hz = modem_mode_band_min_hz(state->selected_mode);
    g_band_max_hz = modem_mode_band_max_hz(state->selected_mode);

    if (!g_lvgl_ready) {
        printf("[render] role=%s mode=%s tone=%dHz peak=%dHz text=%s\r\n",
               is_tx ? "TX" : "RX",
               mode_display_name(state->selected_mode, state->selected_psk_mode),
               state->rx_tone_hz,
               peak_hz,
               state->text[0] ? state->text : "");
        return;
    }

    if (!bsp_display_lock(0)) {
        return;
    }

    const psk_mode_info_t *mode_info = psk_mode_info(state->selected_psk_mode);
    const uint8_t role_changed = (g_last_role != state->display_role);
    const uint8_t mode_changed = (g_last_mode != state->selected_mode ||
                                  g_last_psk_mode != state->selected_psk_mode);
    const uint8_t tone_changed = (g_last_tone_hz != state->rx_tone_hz ||
                                  g_last_peak_hz != peak_hz ||
                                  role_changed);
    const uint8_t input_changed = is_tx ?
        (g_last_tx_active != state->tx_active ||
         g_last_tx_queued != state->tx_queued ||
         role_changed) :
        (g_last_input_abs_avg != state->input_abs_avg ||
         g_last_selected_lane != state->selected_lane ||
         g_last_tx_queued != state->tx_queued ||
         role_changed);
    const uint8_t text_layout_changed = !g_text_cache_valid || g_text_layout_role != state->display_role;
    const uint8_t tx_text_changed = (g_last_tx_text_selection != g_tx_text_selection || role_changed);

    if (role_changed || mode_changed) {
        lv_label_set_text_fmt(g_mode_label, "%s %s %s",
                              is_tx ? "TX" : "RX",
                              mode_info ? psk_mode_family_name(mode_info->family) : "PSK",
                              mode_display_name(state->selected_mode, state->selected_psk_mode));
    }

    if (mode_changed) {
        char line[96];
        format_baud(line, sizeof(line), mode_info);
        lv_label_set_text_fmt(g_baud_label, "%s  %uC %uB%s",
                              line,
                              mode_info ? mode_info->carriers : 0U,
                              mode_info ? mode_info->bits_per_symbol : 0U,
                              mode_info && mode_info->fec ? " FEC" : "");
    }

    if (tone_changed) {
        lv_label_set_text_fmt(g_tone_label, "%s Tone %d Hz Peak %d Hz",
                              is_tx ? "TX" : "RX",
                              state->rx_tone_hz,
                              peak_hz);
    }

    if (input_changed) {
        if (is_tx) {
            lv_label_set_text_fmt(g_input_label, "TX %s  Queue %lu",
                                  state->tx_active ? "ACTIVE" : "IDLE",
                                  (unsigned long)state->tx_queued);
        } else {
            lv_label_set_text_fmt(g_input_label, "Input %u  Lane %c  Queue %lu",
                                  state->input_abs_avg,
                                  state->selected_lane == 1U ? 'R' : 'L',
                                  (unsigned long)state->tx_queued);
        }
    }

    waterfall_push(spectrum);

    if (text_layout_changed) {
        lv_label_set_text(g_text_title_label, is_tx ? "TX TEXT" : "RX TEXT");
        lv_label_set_text(g_mode_panel_title_label, is_tx ? "TX MODE SELECT" : "RX MODE SELECT");
        lv_obj_set_pos(g_text_view, 10, is_tx ? 40 : 32);
        lv_obj_set_height(g_text_view, is_tx ? 88 : 96);
    }

    const char *text = state->text[0] ? tail_text(state->text, TEXT_DISPLAY_TAIL_CHARS) :
        (is_tx ? "[NO TX TEXT QUEUED]" : "[NO DECODED PSK TEXT YET]");
    if (!g_text_cache_valid || strcmp(g_text_cache, text) != 0) {
        strncpy(g_text_cache, text, sizeof(g_text_cache) - 1U);
        g_text_cache[sizeof(g_text_cache) - 1U] = '\0';
        lv_label_set_text(g_rx_label, g_text_cache);
        scroll_text_to_bottom();
        g_text_cache_valid = 1U;
    } else if (text_layout_changed) {
        scroll_text_to_bottom();
    }

    if (role_changed) {
        update_role_buttons(state->display_role);
        update_touch_label();
    }
    if (mode_changed) {
        update_mode_buttons(mode_info);
    }
    if (tx_text_changed) {
        update_tx_text_buttons(state->display_role);
    }
    g_last_role = state->display_role;
    g_last_mode = state->selected_mode;
    g_last_psk_mode = state->selected_psk_mode;
    g_last_tone_hz = state->rx_tone_hz;
    g_last_peak_hz = peak_hz;
    g_last_tx_active = state->tx_active;
    g_last_tx_queued = state->tx_queued;
    g_last_input_abs_avg = state->input_abs_avg;
    g_last_selected_lane = state->selected_lane;
    g_last_tx_text_selection = g_tx_text_selection;
    g_text_layout_role = state->display_role;
    bsp_display_unlock();
}

int display_take_tone_selection(int *tone_hz)
{
    if (!g_has_pending_tone || !tone_hz) {
        return 0;
    }
    *tone_hz = g_pending_tone_hz;
    g_has_pending_tone = 0;
    return 1;
}

static const char *mode_display_name(modem_mode_t mode, psk_mode_t psk_mode)
{
    switch (mode) {
    case MODEM_MODE_BPSK31:
        return "BPSK31";
    case MODEM_MODE_PSK63:
        return psk_mode_name(psk_mode);
    case MODEM_MODE_FT8:
    case MODEM_MODE_FT4:
    case MODEM_MODE_RTTY:
    case MODEM_MODE_CW:
    case MODEM_MODE_MFSK16:
        return modem_mode_name(mode);
    default:
        return "PSK";
    }
}

static int32_t scale_x(int32_t value)
{
    return (int32_t)(((int64_t)value * g_screen_w + 400) / 800);
}

static int32_t scale_y(int32_t value)
{
    return (int32_t)(((int64_t)value * g_screen_h + 240) / 480);
}

static void update_layout_metrics(void)
{
    g_header_h = scale_y(46);
    g_left_panel_x = scale_x(20);
    g_left_panel_y = scale_y(56);
    g_left_panel_w = scale_x(488);
    g_left_panel_h = g_screen_h - g_left_panel_y - scale_y(38);
    g_right_panel_x = scale_x(520);
    g_right_panel_y = scale_y(56);
    g_right_panel_w = g_screen_w - g_right_panel_x - scale_x(20);
    g_right_panel_h = g_left_panel_h;
    g_waterfall_x = scale_x(14);
    g_waterfall_y = scale_y(76);
    g_waterfall_w = scale_x(456);
    g_waterfall_h = scale_y(150);
    g_text_panel_x = scale_x(14);
    g_text_panel_y = scale_y(248);
    g_text_panel_w = scale_x(456);
    g_text_panel_h = scale_y(138);
    g_role_button_x = scale_x(12);
    g_role_button_y = scale_y(10);
    g_role_button_step_x = scale_x(116);
    g_role_button_w = scale_x(108);
    g_role_button_h = scale_y(32);
    g_mode_button_x = scale_x(8);
    g_mode_button_y = scale_y(84);
    g_mode_button_step_x = scale_x(80);
    g_mode_button_step_y = scale_y(36);
    g_mode_button_w = scale_x(76);
    g_mode_button_h = scale_y(31);
    g_tx_text_button_x = scale_x(96);
    g_tx_text_button_y = scale_y(7);
    g_tx_text_button_step_x = scale_x(88);
    g_tx_text_button_w = scale_x(82);
    g_tx_text_button_h = scale_y(25);
}

static void spectrum_touch_cb(lv_event_t *event)
{
    lv_point_t p;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev || !g_waterfall_canvas) {
        return;
    }

    lv_indev_get_point(indev, &p);
    lv_area_t coords;
    lv_obj_get_coords(g_waterfall_canvas, &coords);
    const int width = coords.x2 - coords.x1 + 1;
    int rel_x = p.x - coords.x1;
    if (rel_x < 0) {
        rel_x = 0;
    } else if (rel_x >= width) {
        rel_x = width - 1;
    }

    const int band_min = g_band_min_hz;
    const int band_max = g_band_max_hz;
    g_pending_tone_hz = band_min + (rel_x * (band_max - band_min)) / width;
    g_has_pending_tone = 1;
    (void)event;
}

static void build_ui(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_text_font(screen, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xE8EEF2), LV_PART_MAIN);

    lv_obj_t *header = lv_obj_create(screen);
    lv_obj_remove_style_all(header);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, g_screen_w, g_header_h);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x1E3A5F), LV_PART_MAIN);

    make_label(header, "ESP32-P4 PSK MODEM", scale_x(22), scale_y(10), scale_x(360),
               &lv_font_montserrat_14, lv_color_hex(0xFFFFFF));
    g_touch_label = make_label(header, "TP --", g_screen_w - scale_x(180), scale_y(15), scale_x(170),
                               &lv_font_montserrat_14, lv_color_hex(0xB0BEC5));

    lv_obj_t *left = make_panel(screen, g_left_panel_x, g_left_panel_y, g_left_panel_w, g_left_panel_h);
    g_mode_label = make_label(left, "RX PSK BPSK31", scale_x(14), scale_y(10), scale_x(260),
                              &lv_font_montserrat_14, lv_color_hex(0xFFFFFF));
    g_baud_label = make_label(left, "31.25 BD", scale_x(284), scale_y(13), scale_x(180),
                              &lv_font_montserrat_14, lv_color_hex(0x4DD0E1));
    g_tone_label = make_label(left, "Tone 1500 Hz    Peak 0 Hz", scale_x(14), scale_y(42), scale_x(260),
                              &lv_font_montserrat_14, lv_color_hex(0xFFD54F));
    g_input_label = make_label(left, "Input 0  Lane L  Queue 0", scale_x(284), scale_y(42), scale_x(190),
                               &lv_font_montserrat_14, lv_color_hex(0x81C784));

    g_waterfall_stride = lv_draw_buf_width_to_stride((uint32_t)g_waterfall_w, LV_COLOR_FORMAT_RGB565);
    g_waterfall_buffer_len = (size_t)g_waterfall_stride * (size_t)g_waterfall_h;
    g_waterfall_buffer = heap_caps_malloc(g_waterfall_buffer_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g_waterfall_buffer) {
        g_waterfall_buffer = heap_caps_malloc(g_waterfall_buffer_len, MALLOC_CAP_8BIT);
    }
    if (!g_waterfall_buffer) {
        printf("[display] waterfall alloc failed\r\n");
        return;
    }
    memset(g_waterfall_buffer, 0, g_waterfall_buffer_len);
    g_waterfall_canvas = lv_canvas_create(left);
    lv_canvas_set_buffer(g_waterfall_canvas, g_waterfall_buffer,
                         g_waterfall_w, g_waterfall_h, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(g_waterfall_canvas, g_waterfall_x, g_waterfall_y);
    lv_obj_set_size(g_waterfall_canvas, g_waterfall_w, g_waterfall_h);
    lv_obj_set_style_border_width(g_waterfall_canvas, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(g_waterfall_canvas, lv_color_hex(0x455A64), LV_PART_MAIN);
    lv_obj_add_flag(g_waterfall_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_waterfall_canvas, spectrum_touch_cb, LV_EVENT_CLICKED, NULL);
    make_waterfall_frequency_labels(left);

    lv_obj_t *rx_panel = make_panel(left, g_text_panel_x, g_text_panel_y, g_text_panel_w, g_text_panel_h);
    g_text_title_label = make_label(rx_panel, "RX TEXT", scale_x(10), scale_y(8), scale_x(120),
                                    &lv_font_montserrat_14, lv_color_hex(0x81C784));
    static const char *text_labels[TX_TEXT_BUTTON_COUNT] = {"CQ", "BER", "ASCII", "LOOP"};
    for (size_t i = 0; i < TX_TEXT_BUTTON_COUNT; ++i) {
        lv_obj_t *button = lv_button_create(rx_panel);
        lv_obj_set_pos(button, g_tx_text_button_x + (int32_t)i * g_tx_text_button_step_x,
                       g_tx_text_button_y);
        lv_obj_set_size(button, g_tx_text_button_w, g_tx_text_button_h);
        lv_obj_set_style_radius(button, 4, LV_PART_MAIN);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x26313A), LV_PART_MAIN);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x81C784), LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_set_style_text_color(button, lv_color_hex(0xE8EEF2), LV_PART_MAIN);
        lv_obj_set_style_text_color(button, lv_color_hex(0x101820), LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_add_flag(button, LV_OBJ_FLAG_CHECKABLE);
        lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(button, tx_text_button_event_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);

        lv_obj_t *label = lv_label_create(button);
        lv_label_set_text(label, text_labels[i]);
        lv_obj_center(label);
        g_tx_text_buttons[i] = button;
    }

    g_text_view = lv_obj_create(rx_panel);
    lv_obj_remove_style_all(g_text_view);
    lv_obj_set_pos(g_text_view, scale_x(10), scale_y(32));
    lv_obj_set_size(g_text_view, g_text_panel_w - scale_x(20), scale_y(96));
    lv_obj_set_style_pad_all(g_text_view, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(g_text_view, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_text_view, LV_SCROLLBAR_MODE_AUTO);

    g_rx_label = make_label(g_text_view, "[NO DECODED PSK TEXT YET]", 0, 0, g_text_panel_w - scale_x(24),
                            &lv_font_montserrat_14, lv_color_hex(0xFFFFFF));
    lv_label_set_long_mode(g_rx_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(g_rx_label, LV_SIZE_CONTENT);

    lv_obj_t *right = make_panel(screen, g_right_panel_x, g_right_panel_y, g_right_panel_w, g_right_panel_h);
    static const struct {
        const char *label;
        display_role_t role;
    } roles[] = {
        {"TX", DISPLAY_ROLE_TX},
        {"RX", DISPLAY_ROLE_RX},
    };
    for (size_t i = 0; i < sizeof(roles) / sizeof(roles[0]); ++i) {
        lv_obj_t *button = lv_button_create(right);
        lv_obj_set_pos(button, g_role_button_x + (int32_t)i * g_role_button_step_x, g_role_button_y);
        lv_obj_set_size(button, g_role_button_w, g_role_button_h);
        lv_obj_set_style_radius(button, 4, LV_PART_MAIN);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x26313A), LV_PART_MAIN);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x4DD0E1), LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_set_style_text_color(button, lv_color_hex(0xE8EEF2), LV_PART_MAIN);
        lv_obj_set_style_text_color(button, lv_color_hex(0x101820), LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_add_flag(button, LV_OBJ_FLAG_CHECKABLE);
        lv_obj_add_event_cb(button, role_button_event_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)roles[i].role);

        lv_obj_t *label = lv_label_create(button);
        lv_label_set_text(label, roles[i].label);
        lv_obj_center(label);
        g_role_buttons[roles[i].role == DISPLAY_ROLE_TX ? 0U : 1U] = button;
    }

    g_mode_panel_title_label = make_label(right, "RX MODE SELECT", scale_x(12), scale_y(50), scale_x(220), &lv_font_montserrat_14,
                                          lv_color_hex(0x4DD0E1));

    for (size_t i = 0; i < psk_mode_count() && i < (sizeof(g_mode_buttons) / sizeof(g_mode_buttons[0])); ++i) {
        const psk_mode_info_t *mode = psk_mode_info_at(i);
        const uint32_t col = (uint32_t)(i / MODE_ROWS);
        const uint32_t row = (uint32_t)(i % MODE_ROWS);
        lv_obj_t *button = lv_button_create(right);
        lv_obj_set_pos(button, g_mode_button_x + (int32_t)col * g_mode_button_step_x,
                       g_mode_button_y + (int32_t)row * g_mode_button_step_y);
        lv_obj_set_size(button, g_mode_button_w, g_mode_button_h);
        lv_obj_set_style_radius(button, 4, LV_PART_MAIN);
        lv_obj_set_style_bg_color(button, lv_color_hex(mode && mode->decode_supported ? 0x173322 : 0x26313A),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_color(button, lv_color_hex(0xFFC107), LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_set_style_text_color(button, lv_color_hex(0xE8EEF2), LV_PART_MAIN);
        lv_obj_set_style_text_color(button, lv_color_hex(0x101820), LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_add_flag(button, LV_OBJ_FLAG_CHECKABLE);
        lv_obj_add_event_cb(button, mode_button_event_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)(mode ? mode->id : 0));

        lv_obj_t *label = lv_label_create(button);
        lv_label_set_text(label, mode ? mode->name : "?");
        lv_obj_center(label);
        g_mode_buttons[i] = button;
    }
}

static lv_obj_t *make_panel(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x16212B), LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x455A64), LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 0, LV_PART_MAIN);
    return panel;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, int32_t x, int32_t y, int32_t w,
                            const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, w);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    lv_label_set_text(label, text ? text : "");
    return label;
}

static void role_button_event_cb(lv_event_t *event)
{
    const display_role_t role = (display_role_t)(uintptr_t)lv_event_get_user_data(event);
    if (g_role_cb) {
        g_role_cb(role, g_role_cb_user);
    }
}

static void mode_button_event_cb(lv_event_t *event)
{
    const psk_mode_t mode = (psk_mode_t)(uintptr_t)lv_event_get_user_data(event);
    if (g_mode_cb) {
        g_mode_cb(mode, g_mode_cb_user);
    }
}

static void tx_text_button_event_cb(lv_event_t *event)
{
    const uint8_t index = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    if (g_tx_text_cb) {
        g_tx_text_cb(index, g_tx_text_cb_user);
    }
}

static void update_role_buttons(display_role_t role)
{
    for (size_t i = 0; i < sizeof(g_role_buttons) / sizeof(g_role_buttons[0]); ++i) {
        if (!g_role_buttons[i]) {
            continue;
        }
        const display_role_t button_role = i == 0U ? DISPLAY_ROLE_TX : DISPLAY_ROLE_RX;
        if (button_role == role) {
            lv_obj_add_state(g_role_buttons[i], LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(g_role_buttons[i], LV_STATE_CHECKED);
        }
    }
}

static void update_mode_buttons(const psk_mode_info_t *active_mode)
{
    for (size_t i = 0; i < psk_mode_count() && i < (sizeof(g_mode_buttons) / sizeof(g_mode_buttons[0])); ++i) {
        const psk_mode_info_t *mode = psk_mode_info_at(i);
        if (!mode || !g_mode_buttons[i]) {
            continue;
        }
        if (active_mode && active_mode->id == mode->id) {
            lv_obj_add_state(g_mode_buttons[i], LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(g_mode_buttons[i], LV_STATE_CHECKED);
        }
    }
}

static void update_tx_text_buttons(display_role_t role)
{
    for (size_t i = 0; i < TX_TEXT_BUTTON_COUNT; ++i) {
        if (!g_tx_text_buttons[i]) {
            continue;
        }
        if (role == DISPLAY_ROLE_TX) {
            lv_obj_remove_flag(g_tx_text_buttons[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(g_tx_text_buttons[i], LV_OBJ_FLAG_HIDDEN);
        }
        if (i == g_tx_text_selection) {
            lv_obj_add_state(g_tx_text_buttons[i], LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(g_tx_text_buttons[i], LV_STATE_CHECKED);
        }
    }
}

static void update_touch_label(void)
{
    if (g_touch_label) {
        lv_label_set_text_fmt(g_touch_label, "TP --");
        lv_obj_set_style_text_color(g_touch_label, lv_color_hex(0xB0BEC5), LV_PART_MAIN);
    }
}

static void format_baud(char *buf, size_t len, const psk_mode_info_t *mode)
{
    if (!buf || len == 0U) {
        return;
    }
    if (!mode) {
        snprintf(buf, len, "0 BD");
        return;
    }

    const uint32_t baud_x100 = (uint32_t)(mode->baud * 100.0f + 0.5f);
    if ((baud_x100 % 100U) == 0U) {
        snprintf(buf, len, "%lu BD", (unsigned long)(baud_x100 / 100U));
    } else if ((baud_x100 % 10U) == 0U) {
        snprintf(buf, len, "%lu.%lu BD",
                 (unsigned long)(baud_x100 / 100U),
                 (unsigned long)((baud_x100 / 10U) % 10U));
    } else {
        snprintf(buf, len, "%lu.%02lu BD",
                 (unsigned long)(baud_x100 / 100U),
                 (unsigned long)(baud_x100 % 100U));
    }
}

static const char *tail_text(const char *text, size_t max_len)
{
    const size_t len = text ? strlen(text) : 0U;
    if (!text || len <= max_len) {
        return text ? text : "";
    }
    return text + len - max_len;
}

static void scroll_text_to_bottom(void)
{
    if (!g_text_view) {
        return;
    }
    lv_obj_update_layout(g_text_view);
    lv_obj_scroll_to_y(g_text_view, LV_COORD_MAX, LV_ANIM_OFF);
}

static void make_waterfall_frequency_labels(lv_obj_t *parent)
{
    static const struct {
        uint16_t hz;
        const char *text;
    } ticks[] = {
        {0U, "0 Hz"}, {500U, "500"}, {1000U, "1000"}, {1500U, "1500"},
        {2000U, "2000"}, {2500U, "2500"}, {3000U, "3000 Hz"},
    };

    for (size_t i = 0; i < sizeof(ticks) / sizeof(ticks[0]); ++i) {
        const int32_t center_x = g_waterfall_x +
            (int32_t)(((int64_t)ticks[i].hz * g_waterfall_w) / WATERFALL_MAX_HZ);
        int32_t x = center_x - scale_x(WATERFALL_FREQ_LABEL_WIDTH / 2U);
        if (x < g_waterfall_x) {
            x = g_waterfall_x;
        }
        if (x + scale_x(WATERFALL_FREQ_LABEL_WIDTH) > g_left_panel_x + g_left_panel_w - scale_x(18)) {
            x = g_left_panel_x + g_left_panel_w - scale_x(18 + WATERFALL_FREQ_LABEL_WIDTH);
        }
        lv_obj_t *label = make_label(parent, ticks[i].text, x, scale_y(WATERFALL_LABEL_Y),
                                     scale_x(WATERFALL_FREQ_LABEL_WIDTH), &lv_font_montserrat_14,
                                     lv_color_hex(0x90A4AE));
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    }
}

static void waterfall_push(const spectrum_t *spectrum)
{
    if (!spectrum || !g_waterfall_canvas) {
        return;
    }

    if (!g_waterfall_buffer || g_waterfall_w <= 1 || g_waterfall_h <= 1) {
        return;
    }

    const uint32_t visible_bytes = (uint32_t)g_waterfall_w * sizeof(uint16_t);
    uint8_t *last_row = &g_waterfall_buffer[g_waterfall_stride * (g_waterfall_h - 1U)];

    memmove(g_waterfall_buffer, &g_waterfall_buffer[g_waterfall_stride],
            g_waterfall_stride * (g_waterfall_h - 1U));
    memset(last_row, 0, g_waterfall_stride);

    for (uint32_t x = 0; x < (uint32_t)g_waterfall_w; ++x) {
        const uint32_t scaled = x * ((WATERFALL_MAX_HZ * 2U * SPECTRUM_BINS) / 16000U);
        const uint32_t bin = scaled / ((uint32_t)g_waterfall_w - 1U);
        const uint32_t fraction = scaled % ((uint32_t)g_waterfall_w - 1U);
        const uint32_t next_bin = (bin + 1U < SPECTRUM_BINS) ? bin + 1U : bin;
        const int level0 = spectrum->bins[bin];
        const int level1 = spectrum->bins[next_bin];
        const uint8_t level = (uint8_t)(level0 +
            (((level1 - level0) * (int)fraction) + (int)((uint32_t)g_waterfall_w - 1U) / 2) /
             (int)((uint32_t)g_waterfall_w - 1U));
        const uint16_t color = waterfall_color(level);
        memcpy(&last_row[x * sizeof(color)], &color, sizeof(color));
    }
    if (g_waterfall_stride > visible_bytes) {
        memset(last_row + visible_bytes, 0, g_waterfall_stride - visible_bytes);
    }

    lv_obj_invalidate(g_waterfall_canvas);
}

static uint16_t waterfall_color(uint8_t level)
{
    uint8_t red = 0U;
    uint8_t green = 0U;
    uint8_t blue = 0U;

    if (level < 16U) {
        blue = (uint8_t)(level * 2U);
    } else if (level < 64U) {
        blue = (uint8_t)(32U + (level - 16U) * 4U);
    } else if (level < 128U) {
        green = (uint8_t)((level - 64U) * 4U);
        blue = (uint8_t)(255U - (level - 64U) * 3U);
    } else if (level < 192U) {
        red = (uint8_t)((level - 128U) * 4U);
        green = 255U;
    } else {
        red = 255U;
        green = 255U;
        blue = (uint8_t)((level - 192U) * 4U);
    }

    return lv_color_to_u16(lv_color_make(red, green, blue));
}
