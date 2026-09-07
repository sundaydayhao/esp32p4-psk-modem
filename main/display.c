#include "display.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board_config.h"
#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"
#include "psk_modes.h"

static const char *TAG = "display";

#define UI_BASE_WIDTH 800
#define UI_BASE_HEIGHT 480
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
#define WATERFALL_MAX_HZ SPECTRUM_BINS
#define WATERFALL_LABEL_Y 229U
#define WATERFALL_FREQ_LABEL_WIDTH 48U
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

typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} waterfall_palette_t;

static lv_obj_t *s_mode_label;
static lv_obj_t *s_baud_label;
static lv_obj_t *s_tone_label;
static lv_obj_t *s_input_label;
static lv_obj_t *s_touch_label;
static lv_obj_t *s_text_view;
static lv_obj_t *s_text_label;
static lv_obj_t *s_text_title_label;
static lv_obj_t *s_mode_panel_title_label;
static lv_obj_t *s_waterfall_canvas;
static uint8_t *s_waterfall_buffer;
static lv_obj_t *s_role_buttons[2];
static lv_obj_t *s_mode_buttons[32];
static lv_obj_t *s_tx_text_buttons[TX_TEXT_BUTTON_COUNT];
static int s_lcd_w;
static int s_lcd_h;
static int s_ui_w;
static int s_ui_h;
static int s_waterfall_w;
static int s_waterfall_h;
static int s_waterfall_x;
static int s_waterfall_y;
static int s_waterfall_label_y;
static int s_waterfall_freq_label_w;
static int s_current_role;
static uint8_t s_tx_text_selection;
static char s_text_cache[TEXT_DISPLAY_CACHE_LEN];
static uint8_t s_text_cache_valid;
static int s_text_layout_role;
static int s_pending_tone_hz;
static int s_has_pending_tone;
static psk_mode_t s_pending_mode;
static int s_has_pending_mode;
static int s_pending_role;
static int s_has_pending_role;
static uint8_t s_pending_tx_text_index;
static int s_has_pending_tx_text;
static uint16_t s_touch_x;
static uint16_t s_touch_y;
static uint8_t s_touch_seen;

static const waterfall_palette_t s_waterfall_palette[9] = {
    {  0U,   0U,   0U},
    {  0U,   0U, 136U},
    {  0U,  19U, 198U},
    {  0U,  32U, 239U},
    {172U, 167U, 105U},
    {195U, 198U,  49U},
    {225U, 228U, 107U},
    {255U, 255U,   0U},
    {251U,  51U,   0U},
};

static void build_ui(lv_display_t *disp);
static int scale_x(int value);
static int scale_y(int value);
static lv_obj_t *make_panel(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h);
static lv_obj_t *make_label(lv_obj_t *parent, const char *text, int32_t x, int32_t y,
                            int32_t w, const lv_font_t *font, lv_color_t color);
static void role_button_event_cb(lv_event_t *event);
static void mode_button_event_cb(lv_event_t *event);
static void tx_text_button_event_cb(lv_event_t *event);
static void waterfall_touch_cb(lv_event_t *event);
static void update_role_buttons(int role);
static void update_mode_buttons(const psk_mode_info_t *active_mode);
static void update_tx_text_buttons(int role);
static void update_touch_label(void);
static void format_baud(char *buf, size_t len, const psk_mode_info_t *mode);
static const char *tail_text(const char *text, size_t max_len);
static void scroll_text_to_bottom(void);
static void make_waterfall_frequency_labels(lv_obj_t *parent);
static void waterfall_push(const spectrum_t *spectrum);
static uint16_t waterfall_color(uint8_t level);
static void remember_touch_point(void);

esp_err_t display_init(void)
{
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .hw_cfg = {
#if CONFIG_BSP_LCD_TYPE_HDMI
#if CONFIG_BSP_LCD_HDMI_800x600_60HZ
            .hdmi_resolution = BSP_HDMI_RES_800x600,
#elif CONFIG_BSP_LCD_HDMI_1280x720_60HZ
            .hdmi_resolution = BSP_HDMI_RES_1280x720,
#elif CONFIG_BSP_LCD_HDMI_1280x800_60HZ
            .hdmi_resolution = BSP_HDMI_RES_1280x800,
#elif CONFIG_BSP_LCD_HDMI_1920x1080_30HZ
            .hdmi_resolution = BSP_HDMI_RES_1920x1080,
#endif
#else
            .hdmi_resolution = BSP_HDMI_RES_NONE,
#endif
            .dsi_bus = {
                .phy_clk_src = 0,
                .lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS,
            },
        },
        .flags = {
#if CONFIG_BSP_LCD_COLOR_FORMAT_RGB888
            .buff_dma = false,
#else
            .buff_dma = true,
#endif
            .buff_spiram = true,
            .sw_rotate = true,
        },
    };
    cfg.lvgl_port_cfg.task_affinity = 1;
    lv_display_t *disp = bsp_display_start_with_config(&cfg);
    if (!disp) {
        return ESP_FAIL;
    }

    s_lcd_w = (int)lv_display_get_horizontal_resolution(disp);
    s_lcd_h = (int)lv_display_get_vertical_resolution(disp);
    if (s_lcd_w <= 0 || s_lcd_h <= 0) {
        s_lcd_w = BOARD_LCD_WIDTH;
        s_lcd_h = BOARD_LCD_HEIGHT;
    }
    s_ui_w = s_lcd_w > 0 ? s_lcd_w : UI_BASE_WIDTH;
    s_ui_h = s_lcd_h > 0 ? s_lcd_h : UI_BASE_HEIGHT;
    s_waterfall_x = scale_x(WATERFALL_X);
    s_waterfall_y = scale_y(WATERFALL_Y);
    s_waterfall_w = scale_x(WATERFALL_WIDTH);
    s_waterfall_h = scale_y(WATERFALL_HEIGHT);
    s_waterfall_label_y = scale_y(WATERFALL_LABEL_Y);
    s_waterfall_freq_label_w = scale_x(WATERFALL_FREQ_LABEL_WIDTH);
    if (s_waterfall_w < 2) {
        s_waterfall_w = 2;
    }
    if (s_waterfall_h < 2) {
        s_waterfall_h = 2;
    }
    if (s_waterfall_freq_label_w < 24) {
        s_waterfall_freq_label_w = 24;
    }

    const size_t waterfall_bytes = LV_CANVAS_BUF_SIZE(s_waterfall_w, s_waterfall_h,
                                                      WATERFALL_COLOR_BITS,
                                                      LV_DRAW_BUF_STRIDE_ALIGN);
    s_waterfall_buffer = heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, waterfall_bytes,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_waterfall_buffer) {
        s_waterfall_buffer = heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, waterfall_bytes,
                                                     MALLOC_CAP_8BIT);
    }
    if (!s_waterfall_buffer) {
        return ESP_ERR_NO_MEM;
    }
    memset(s_waterfall_buffer, 0, waterfall_bytes);

    if (bsp_display_lock(0)) {
        build_ui(disp);
        bsp_display_unlock();
    }
    bsp_display_backlight_on();

    ESP_LOGI(TAG, "ESP32-P4 BSP LCD/touch ready: %dx%d, HPM6750 UI scaled to %dx%d",
             s_lcd_w, s_lcd_h, s_ui_w, s_ui_h);
    return ESP_OK;
}

void display_append_text(display_state_t *state, const char *text)
{
    const size_t add = strlen(text);
    if (add == 0) {
        return;
    }
    if (add >= sizeof(state->text)) {
        text += add - sizeof(state->text) + 1;
    }

    size_t keep = state->text_len;
    const size_t incoming = strlen(text);
    if (keep + incoming >= sizeof(state->text)) {
        const size_t drop = keep + incoming - sizeof(state->text) + 1;
        memmove(state->text, state->text + drop, keep - drop);
        keep -= drop;
    }
    memcpy(state->text + keep, text, incoming + 1);
    state->text_len = keep + incoming;
}

void display_render(const display_state_t *state, const spectrum_t *spectrum)
{
    if (!s_waterfall_canvas || !s_waterfall_buffer || !state) {
        return;
    }

    char line[96];
    const int is_tx = state->display_role != 0;
    const psk_mode_info_t *mode = psk_mode_info(is_tx ? state->selected_tx_psk_mode :
                                                state->selected_psk_mode);
    const int tone_hz = is_tx ? state->tx_tone_hz : state->rx_tone_hz;
    const int peak_hz = spectrum ? spectrum->peak_hz : 0;

    if (bsp_display_lock(0)) {
        s_current_role = is_tx;
        lv_label_set_text_fmt(s_mode_label, "%s %s %s",
                              is_tx ? "TX" : "RX",
                              mode ? psk_mode_family_name(mode->family) : "PSK",
                              mode ? mode->name : "");
        format_baud(line, sizeof(line), mode);
        lv_label_set_text_fmt(s_baud_label, "%s  %uC %uB%s",
                              line,
                              mode ? mode->carriers : 0U,
                              mode ? mode->bits_per_symbol : 0U,
                              mode && mode->fec ? " FEC" :
                              (mode && !mode->decode_supported ? " VIEW" : ""));
        lv_label_set_text_fmt(s_tone_label, "%s Tone %d Hz Peak %d Hz %+.0f dBFS",
                              is_tx ? "TX" : "RX",
                              tone_hz,
                              peak_hz,
                              spectrum ? spectrum->peak_dbfs : -120.0f);
        if (is_tx) {
            lv_label_set_text_fmt(s_input_label, "TX %s  Queue %u",
                                  state->tx_active ? "ACTIVE" : "IDLE",
                                  (unsigned)state->tx_queued);
        } else {
            lv_label_set_text_fmt(s_input_label, "Input %u  Lane %c  Queue %u",
                                  (unsigned)state->input_abs_avg,
                                  state->selected_lane == 1U ? 'R' : 'L',
                                  (unsigned)state->tx_queued);
        }

        if (state->update_waterfall) {
            waterfall_push(spectrum);
        }

        const uint8_t text_layout_changed = !s_text_cache_valid || s_text_layout_role != is_tx;
        if (text_layout_changed) {
            lv_label_set_text(s_text_title_label, is_tx ? "TX TEXT" : "RX TEXT");
            lv_label_set_text(s_mode_panel_title_label, is_tx ? "TX MODE SELECT" : "RX MODE SELECT");
            lv_obj_set_pos(s_text_view, scale_x(10), scale_y(is_tx ? 40 : 32));
            lv_obj_set_height(s_text_view, scale_y(is_tx ? 88 : 96));
            s_text_layout_role = is_tx;
        }

        const char *display_text = is_tx ? state->tx_text : state->text;
        display_text = display_text && display_text[0] ? tail_text(display_text, TEXT_DISPLAY_TAIL_CHARS) :
            (is_tx ? "[NO TX TEXT QUEUED]" : "[NO DECODED PSK TEXT YET]");
        if (!s_text_cache_valid || strcmp(s_text_cache, display_text) != 0) {
            strncpy(s_text_cache, display_text, sizeof(s_text_cache) - 1U);
            s_text_cache[sizeof(s_text_cache) - 1U] = '\0';
            lv_label_set_text(s_text_label, s_text_cache);
            scroll_text_to_bottom();
            s_text_cache_valid = 1U;
        } else if (text_layout_changed) {
            scroll_text_to_bottom();
        }

        update_touch_label();
        update_role_buttons(is_tx);
        update_mode_buttons(mode);
        update_tx_text_buttons(is_tx);
        bsp_display_unlock();
    }
}

int display_take_tone_selection(int *tone_hz)
{
    if (!s_has_pending_tone) {
        return 0;
    }
    if (tone_hz) {
        *tone_hz = s_pending_tone_hz;
    }
    s_has_pending_tone = 0;
    return 1;
}

int display_take_mode_selection(psk_mode_t *mode)
{
    if (!s_has_pending_mode) {
        return 0;
    }
    if (mode) {
        *mode = s_pending_mode;
    }
    s_has_pending_mode = 0;
    return 1;
}

int display_take_role_selection(int *display_role)
{
    if (!s_has_pending_role) {
        return 0;
    }
    if (display_role) {
        *display_role = s_pending_role;
    }
    s_has_pending_role = 0;
    return 1;
}

int display_take_tx_text_selection(uint8_t *index)
{
    if (!s_has_pending_tx_text) {
        return 0;
    }
    if (index) {
        *index = s_pending_tx_text_index;
    }
    s_has_pending_tx_text = 0;
    return 1;
}

void display_set_tx_text_selection(uint8_t index)
{
    if (index < TX_TEXT_BUTTON_COUNT) {
        s_tx_text_selection = index;
    }
}

void display_task_handler(void)
{
}

static int scale_x(int value)
{
    return (int)(((int64_t)value * (int64_t)s_ui_w + UI_BASE_WIDTH / 2) / UI_BASE_WIDTH);
}

static int scale_y(int value)
{
    return (int)(((int64_t)value * (int64_t)s_ui_h + UI_BASE_HEIGHT / 2) / UI_BASE_HEIGHT);
}

static void build_ui(lv_display_t *disp)
{
    lv_obj_t *screen = lv_display_get_screen_active(disp);
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_text_font(screen, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xE8EEF2), LV_PART_MAIN);

    lv_obj_t *root = lv_obj_create(screen);
    lv_obj_remove_style_all(root);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_size(root, s_ui_w, s_ui_h);

    lv_obj_t *header = lv_obj_create(root);
    lv_obj_remove_style_all(header);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, s_ui_w, scale_y(46));
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x1E3A5F), LV_PART_MAIN);

    make_label(header, "HPM6750 PSK MODEM", scale_x(22), scale_y(10), scale_x(360),
               &lv_font_montserrat_14,
               lv_color_hex(0xFFFFFF));
    s_touch_label = make_label(header, "TP --", scale_x(610), scale_y(15), scale_x(170),
                               &lv_font_montserrat_14,
                               lv_color_hex(0xB0BEC5));

    lv_obj_t *left = make_panel(root, scale_x(LEFT_PANEL_X), scale_y(LEFT_PANEL_Y),
                                scale_x(488), scale_y(406));
    s_mode_label = make_label(left, "RX BPSK BPSK31", scale_x(14), scale_y(10),
                              scale_x(260), &lv_font_montserrat_14,
                              lv_color_hex(0xFFFFFF));
    s_baud_label = make_label(left, "31.25 BD", scale_x(284), scale_y(13), scale_x(180),
                              &lv_font_montserrat_14,
                              lv_color_hex(0x4DD0E1));
    s_tone_label = make_label(left, "RX Tone 1500 Hz Peak 0 Hz", scale_x(14), scale_y(42),
                              scale_x(260),
                              &lv_font_montserrat_14, lv_color_hex(0xFFD54F));
    s_input_label = make_label(left, "Input 0  Lane L  Queue 0", scale_x(284), scale_y(42),
                               scale_x(190),
                               &lv_font_montserrat_14, lv_color_hex(0x81C784));

    s_waterfall_canvas = lv_canvas_create(left);
    lv_canvas_set_buffer(s_waterfall_canvas, s_waterfall_buffer,
                         s_waterfall_w, s_waterfall_h, LV_COLOR_FORMAT_RGB565);
    lv_canvas_fill_bg(s_waterfall_canvas, lv_color_hex(0x000000), LV_OPA_COVER);
    lv_obj_set_pos(s_waterfall_canvas, s_waterfall_x, s_waterfall_y);
    lv_obj_set_size(s_waterfall_canvas, s_waterfall_w, s_waterfall_h);
    lv_obj_set_style_border_width(s_waterfall_canvas, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_waterfall_canvas, lv_color_hex(0x455A64), LV_PART_MAIN);
    lv_obj_add_flag(s_waterfall_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_waterfall_canvas, waterfall_touch_cb, LV_EVENT_CLICKED, NULL);
    make_waterfall_frequency_labels(left);

    lv_obj_t *text_panel = make_panel(left, scale_x(TEXT_PANEL_X), scale_y(TEXT_PANEL_Y),
                                      scale_x(456), scale_y(138));
    s_text_title_label = make_label(text_panel, "RX TEXT", scale_x(10), scale_y(8),
                                    scale_x(120), &lv_font_montserrat_14,
                                    lv_color_hex(0x81C784));
    static const char *text_labels[TX_TEXT_BUTTON_COUNT] = {"CQ", "BER", "ASCII", "LOOP"};
    for (size_t i = 0; i < TX_TEXT_BUTTON_COUNT; ++i) {
        lv_obj_t *button = lv_button_create(text_panel);
        lv_obj_set_pos(button, scale_x((int)(TX_TEXT_BUTTON_X + i * TX_TEXT_BUTTON_STEP_X)),
                       scale_y(TX_TEXT_BUTTON_Y));
        lv_obj_set_size(button, scale_x(TX_TEXT_BUTTON_W), scale_y(TX_TEXT_BUTTON_H));
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
        s_tx_text_buttons[i] = button;
    }

    s_text_view = lv_obj_create(text_panel);
    lv_obj_remove_style_all(s_text_view);
    lv_obj_set_pos(s_text_view, scale_x(10), scale_y(32));
    lv_obj_set_size(s_text_view, scale_x(436), scale_y(96));
    lv_obj_set_style_pad_all(s_text_view, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(s_text_view, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_text_view, LV_SCROLLBAR_MODE_AUTO);

    s_text_label = make_label(s_text_view, "[NO DECODED PSK TEXT YET]", 0, 0, scale_x(424),
                              &lv_font_montserrat_14, lv_color_hex(0xFFFFFF));
    lv_label_set_long_mode(s_text_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(s_text_label, LV_SIZE_CONTENT);

    lv_obj_t *right = make_panel(root, scale_x(MODE_PANEL_X), scale_y(MODE_PANEL_Y),
                                 scale_x(256), scale_y(406));
    static const struct {
        const char *label;
        int role;
    } roles[] = {
        {"TX", 1},
        {"RX", 0},
    };
    for (size_t i = 0; i < sizeof(roles) / sizeof(roles[0]); ++i) {
        lv_obj_t *button = lv_button_create(right);
        lv_obj_set_pos(button, scale_x((int)(ROLE_BUTTON_X + i * ROLE_BUTTON_STEP_X)),
                       scale_y(ROLE_BUTTON_Y));
        lv_obj_set_size(button, scale_x(ROLE_BUTTON_W), scale_y(ROLE_BUTTON_H));
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
        s_role_buttons[roles[i].role ? 0U : 1U] = button;
    }
    s_mode_panel_title_label = make_label(right, "RX MODE SELECT", scale_x(12), scale_y(50),
                                          scale_x(220),
                                          &lv_font_montserrat_14, lv_color_hex(0x4DD0E1));

    for (size_t i = 0; i < psk_mode_count() && i < (sizeof(s_mode_buttons) / sizeof(s_mode_buttons[0])); ++i) {
        const psk_mode_info_t *mode = psk_mode_info_at(i);
        const uint32_t col = (uint32_t)(i / MODE_ROWS);
        const uint32_t row = (uint32_t)(i % MODE_ROWS);
        lv_obj_t *button = lv_button_create(right);
        lv_obj_set_pos(button, scale_x((int)(MODE_BUTTON_X + col * MODE_BUTTON_STEP_X)),
                       scale_y((int)(MODE_BUTTON_Y + row * MODE_BUTTON_STEP_Y)));
        lv_obj_set_size(button, scale_x(MODE_BUTTON_W), scale_y(MODE_BUTTON_H));
        lv_obj_set_style_radius(button, 4, LV_PART_MAIN);
        lv_obj_set_style_bg_color(button,
                                  lv_color_hex(mode && mode->decode_supported ? 0x173322 : 0x26313A),
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
        s_mode_buttons[i] = button;
    }

    update_role_buttons(0);
    update_mode_buttons(psk_mode_info(PSK_MODE_BPSK31));
    update_tx_text_buttons(0);
    update_touch_label();
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

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, int32_t x, int32_t y,
                            int32_t w, const lv_font_t *font, lv_color_t color)
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
    remember_touch_point();
    s_pending_role = (int)(uintptr_t)lv_event_get_user_data(event);
    s_has_pending_role = 1;
    s_current_role = s_pending_role;
    update_role_buttons(s_current_role);
    update_tx_text_buttons(s_current_role);
}

static void mode_button_event_cb(lv_event_t *event)
{
    remember_touch_point();
    s_pending_mode = (psk_mode_t)(uintptr_t)lv_event_get_user_data(event);
    s_has_pending_mode = 1;
}

static void tx_text_button_event_cb(lv_event_t *event)
{
    remember_touch_point();
    s_pending_tx_text_index = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    s_has_pending_tx_text = 1;
    s_tx_text_selection = s_pending_tx_text_index;
    update_tx_text_buttons(1);
}

static void waterfall_touch_cb(lv_event_t *event)
{
    (void)event;
    remember_touch_point();

    lv_indev_t *indev = lv_indev_get_act();
    if (!indev || !s_waterfall_canvas) {
        return;
    }

    lv_point_t p;
    lv_area_t coords;
    lv_indev_get_point(indev, &p);
    lv_obj_get_coords(s_waterfall_canvas, &coords);

    const int width = coords.x2 - coords.x1 + 1;
    int rel_x = p.x - coords.x1;
    if (rel_x < 0) {
        rel_x = 0;
    } else if (rel_x >= width) {
        rel_x = width - 1;
    }

    s_pending_tone_hz = (rel_x * (int)WATERFALL_MAX_HZ) / width;
    s_has_pending_tone = 1;
}

static void update_role_buttons(int role)
{
    for (size_t i = 0; i < sizeof(s_role_buttons) / sizeof(s_role_buttons[0]); ++i) {
        if (!s_role_buttons[i]) {
            continue;
        }
        const int button_role = i == 0U ? 1 : 0;
        if (button_role == role) {
            lv_obj_add_state(s_role_buttons[i], LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(s_role_buttons[i], LV_STATE_CHECKED);
        }
    }
}

static void update_mode_buttons(const psk_mode_info_t *active_mode)
{
    for (size_t i = 0; i < psk_mode_count() && i < (sizeof(s_mode_buttons) / sizeof(s_mode_buttons[0])); ++i) {
        const psk_mode_info_t *mode = psk_mode_info_at(i);
        if (!mode || !s_mode_buttons[i]) {
            continue;
        }
        if (active_mode && active_mode->id == mode->id) {
            lv_obj_add_state(s_mode_buttons[i], LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(s_mode_buttons[i], LV_STATE_CHECKED);
        }
    }
}

static void update_tx_text_buttons(int role)
{
    for (size_t i = 0; i < TX_TEXT_BUTTON_COUNT; ++i) {
        if (!s_tx_text_buttons[i]) {
            continue;
        }
        if (role) {
            lv_obj_remove_flag(s_tx_text_buttons[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_tx_text_buttons[i], LV_OBJ_FLAG_HIDDEN);
        }
        if (i == s_tx_text_selection) {
            lv_obj_add_state(s_tx_text_buttons[i], LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(s_tx_text_buttons[i], LV_STATE_CHECKED);
        }
    }
}

static void update_touch_label(void)
{
    if (!s_touch_label) {
        return;
    }
    if (s_touch_seen) {
        lv_label_set_text_fmt(s_touch_label, "TP OK %u,%u", s_touch_x, s_touch_y);
        lv_obj_set_style_text_color(s_touch_label, lv_color_hex(0x4DD0E1), LV_PART_MAIN);
    } else {
        lv_label_set_text(s_touch_label, "TP OK");
        lv_obj_set_style_text_color(s_touch_label, lv_color_hex(0x4DD0E1), LV_PART_MAIN);
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
        snprintf(buf, len, "%u BD", (unsigned)(baud_x100 / 100U));
    } else if ((baud_x100 % 10U) == 0U) {
        snprintf(buf, len, "%u.%u BD",
                 (unsigned)(baud_x100 / 100U),
                 (unsigned)((baud_x100 / 10U) % 10U));
    } else {
        snprintf(buf, len, "%u.%02u BD",
                 (unsigned)(baud_x100 / 100U),
                 (unsigned)(baud_x100 % 100U));
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
    if (!s_text_view) {
        return;
    }
    lv_obj_update_layout(s_text_view);
    lv_obj_scroll_to_y(s_text_view, LV_COORD_MAX, LV_ANIM_OFF);
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
        const int32_t center_x = s_waterfall_x +
            (int32_t)(((uint32_t)ticks[i].hz * (uint32_t)s_waterfall_w) / WATERFALL_MAX_HZ);
        const int32_t max_right = s_waterfall_x + s_waterfall_w;
        int32_t x = center_x - (s_waterfall_freq_label_w / 2);
        if (x < s_waterfall_x) {
            x = s_waterfall_x;
        }
        if (x + s_waterfall_freq_label_w > max_right) {
            x = max_right - s_waterfall_freq_label_w;
        }
        lv_obj_t *label = make_label(parent, ticks[i].text, x, s_waterfall_label_y,
                                     s_waterfall_freq_label_w, &lv_font_montserrat_14,
                                     lv_color_hex(0x90A4AE));
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    }
}

static void waterfall_push(const spectrum_t *spectrum)
{
    if (!spectrum || !s_waterfall_canvas || !s_waterfall_buffer) {
        return;
    }

    const uint32_t stride = lv_draw_buf_width_to_stride((uint32_t)s_waterfall_w,
                                                        LV_COLOR_FORMAT_RGB565);
    const uint32_t visible_bytes = (uint32_t)s_waterfall_w * sizeof(uint16_t);
    uint8_t *first_row = s_waterfall_buffer;

    memmove(&s_waterfall_buffer[stride], s_waterfall_buffer,
            stride * (uint32_t)(s_waterfall_h - 1));
    memset(first_row, 0, stride);

    for (uint32_t x = 0; x < (uint32_t)s_waterfall_w; ++x) {
        const uint64_t numerator = (uint64_t)x * (SPECTRUM_BINS - 1U);
        const uint64_t denominator = (uint64_t)(s_waterfall_w - 1);
        uint32_t bin = (uint32_t)(numerator / denominator);
        uint64_t fraction = numerator % denominator;
        if (bin >= SPECTRUM_BINS) {
            bin = SPECTRUM_BINS - 1U;
            fraction = 0U;
        }
        const uint32_t next_bin = (bin + 1U < SPECTRUM_BINS) ? bin + 1U : bin;
        const int level0 = spectrum->bins[bin];
        const int level1 = spectrum->bins[next_bin];
        const uint8_t level = (uint8_t)(level0 +
            ((int64_t)(level1 - level0) * (int64_t)fraction + (int64_t)denominator / 2) /
             (int64_t)denominator);
        const uint16_t color = waterfall_color(level);
        memcpy(&first_row[x * sizeof(color)], &color, sizeof(color));
    }
    if (stride > visible_bytes) {
        memset(first_row + visible_bytes, 0, stride - visible_bytes);
    }

    lv_obj_invalidate(s_waterfall_canvas);
}

static uint16_t waterfall_color(uint8_t level)
{
    uint32_t section = level / 32U;
    const uint32_t step = level % 32U;
    if (section >= 8U) {
        section = 7U;
    }

    const waterfall_palette_t *a = &s_waterfall_palette[section];
    const waterfall_palette_t *b = &s_waterfall_palette[section + 1U];
    const uint8_t red = (uint8_t)((int32_t)a->red +
        ((int32_t)step * ((int32_t)b->red - (int32_t)a->red)) / 32);
    const uint8_t green = (uint8_t)((int32_t)a->green +
        ((int32_t)step * ((int32_t)b->green - (int32_t)a->green)) / 32);
    const uint8_t blue = (uint8_t)((int32_t)a->blue +
        ((int32_t)step * ((int32_t)b->blue - (int32_t)a->blue)) / 32);

    return lv_color_to_u16(lv_color_make(red, green, blue));
}

static void remember_touch_point(void)
{
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) {
        return;
    }
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    s_touch_x = point.x < 0 ? 0U : (uint16_t)point.x;
    s_touch_y = point.y < 0 ? 0U : (uint16_t)point.y;
    s_touch_seen = 1U;
    update_touch_label();
}
