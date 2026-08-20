#include "display.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "board_config.h"
#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "display";

static lv_obj_t *s_spectrum;
static lv_obj_t *s_tone_marker;
static lv_obj_t *s_text_view;
static lv_obj_t *s_text;
static lv_obj_t *s_status;
static lv_color_t *s_canvas_buf;
static int s_lcd_w;
static int s_lcd_h;
static int s_waterfall_h;
static int s_pending_tone_hz;
static int s_has_pending_tone;
static int s_band_min_hz = 100;
static int s_band_max_hz = 3500;

#define WATERFALL_ROWS_PER_FRAME 4

static void spectrum_touch_cb(lv_event_t *event)
{
    lv_point_t p;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev || !s_spectrum) {
        return;
    }

    lv_indev_get_point(indev, &p);
    lv_area_t coords;
    lv_obj_get_coords(s_spectrum, &coords);
    const int width = coords.x2 - coords.x1 + 1;
    int rel_x = p.x - coords.x1;
    if (rel_x < 0) {
        rel_x = 0;
    } else if (rel_x >= width) {
        rel_x = width - 1;
    }
    s_pending_tone_hz = s_band_min_hz + (rel_x * (s_band_max_hz - s_band_min_hz)) / width;
    s_has_pending_tone = 1;
    (void)event;
}

static lv_color_t waterfall_color(uint8_t power)
{
    if (power < 12) {
        return lv_color_hex(0x000000);
    }
    if (power < 96) {
        const uint8_t b = 48 + (power * 160) / 96;
        return lv_color_make(0, 0, b);
    }
    if (power < 170) {
        const uint8_t t = (uint8_t)(((power - 96) * 255) / 74);
        return lv_color_make(t / 4, t / 4, 210);
    }
    if (power < 230) {
        const uint8_t t = (uint8_t)(((power - 170) * 255) / 60);
        return lv_color_make(40 + t, 40 + t, 210 - (t * 170) / 255);
    }
    return lv_color_make(255, 245, 160);
}

static uint8_t waterfall_power_at_x(const spectrum_t *spectrum, int x)
{
    const int nyquist_hz = BOARD_MODEM_SAMPLE_RATE_HZ / 2;
    const int width = s_lcd_w > 1 ? s_lcd_w : 2;
    const int freq_hz = s_band_min_hz + (x * (s_band_max_hz - s_band_min_hz)) / (width - 1);
    const int scaled = freq_hz * (SPECTRUM_BINS - 1) * 256 / nyquist_hz;
    int bin = scaled / 256;
    if (bin < 0) {
        bin = 0;
    } else if (bin >= SPECTRUM_BINS) {
        bin = SPECTRUM_BINS - 1;
    }
    const int frac = scaled % 256;
    const int next = bin + 1 < SPECTRUM_BINS ? bin + 1 : bin;
    const int a = spectrum->bins[bin];
    const int b = spectrum->bins[next];
    return (uint8_t)((a * (256 - frac) + b * frac) / 256);
}

static void waterfall_render_row(lv_color_t *buf, const spectrum_t *spectrum)
{
    const int rows = WATERFALL_ROWS_PER_FRAME < s_waterfall_h ? WATERFALL_ROWS_PER_FRAME : 1;
    memmove(buf + s_lcd_w * rows, buf, s_lcd_w * (s_waterfall_h - rows) * sizeof(buf[0]));

    for (int row = 0; row < rows; ++row) {
        lv_color_t *line = buf + row * s_lcd_w;
        for (int x = 0; x < s_lcd_w; ++x) {
            line[x] = waterfall_color(waterfall_power_at_x(spectrum, x));
        }
    }
}

static void update_tone_marker(int rx_tone_hz)
{
    int marker_x = ((rx_tone_hz - s_band_min_hz) * s_lcd_w) / (s_band_max_hz - s_band_min_hz);
    if (marker_x < 0) {
        marker_x = 0;
    } else if (marker_x >= s_lcd_w) {
        marker_x = s_lcd_w - 1;
    }

    if (s_tone_marker) {
        lv_area_t coords;
        lv_obj_get_coords(s_spectrum, &coords);
        lv_obj_set_pos(s_tone_marker, coords.x1 + marker_x, coords.y1);
    }
}

static void create_ui(lv_display_t *disp)
{
    lv_obj_t *screen = lv_display_get_screen_active(disp);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101418), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xe8ecef), 0);

    s_status = lv_label_create(screen);
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 10, 8);

    s_spectrum = lv_canvas_create(screen);
    lv_obj_set_size(s_spectrum, s_lcd_w, s_waterfall_h);
    lv_obj_align(s_spectrum, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_add_flag(s_spectrum, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_spectrum, spectrum_touch_cb, LV_EVENT_CLICKED, NULL);

    s_tone_marker = lv_obj_create(screen);
    lv_obj_set_size(s_tone_marker, 3, s_waterfall_h);
    lv_obj_set_style_bg_color(s_tone_marker, lv_color_hex(0xff1b1b), 0);
    lv_obj_set_style_bg_opa(s_tone_marker, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_tone_marker, 0, 0);
    lv_obj_set_style_radius(s_tone_marker, 0, 0);
    lv_obj_clear_flag(s_tone_marker, LV_OBJ_FLAG_CLICKABLE);

    s_text_view = lv_obj_create(screen);
    lv_obj_set_size(s_text_view, s_lcd_w - 24, s_lcd_h - s_waterfall_h - 58);
    lv_obj_align(s_text_view, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_opa(s_text_view, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_text_view, 0, 0);
    lv_obj_set_style_pad_all(s_text_view, 0, 0);
    lv_obj_set_scroll_dir(s_text_view, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_text_view, LV_SCROLLBAR_MODE_ACTIVE);

    s_text = lv_label_create(s_text_view);
    lv_obj_set_width(s_text, s_lcd_w - 24);
    lv_label_set_long_mode(s_text, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_text, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text(s_text, "");
}

esp_err_t display_init(void)
{
    lv_display_t *disp = bsp_display_start();
    if (!disp) {
        return ESP_FAIL;
    }

    s_lcd_w = (int)lv_display_get_horizontal_resolution(disp);
    s_lcd_h = (int)lv_display_get_vertical_resolution(disp);
    if (s_lcd_w <= 0 || s_lcd_h <= 0) {
        s_lcd_w = BOARD_LCD_WIDTH;
        s_lcd_h = BOARD_LCD_HEIGHT;
    }
    s_waterfall_h = s_lcd_h / 3;
    if (s_waterfall_h < 120) {
        s_waterfall_h = 120;
    } else if (s_waterfall_h > 320) {
        s_waterfall_h = 320;
    }

    const size_t canvas_pixels = (size_t)s_lcd_w * (size_t)s_waterfall_h;
    s_canvas_buf = heap_caps_malloc(canvas_pixels * sizeof(lv_color_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_canvas_buf) {
        s_canvas_buf = heap_caps_malloc(canvas_pixels * sizeof(lv_color_t), MALLOC_CAP_8BIT);
    }
    assert(s_canvas_buf);

    if (bsp_display_lock(0)) {
        create_ui(disp);
        lv_canvas_set_buffer(s_spectrum, s_canvas_buf, s_lcd_w, s_waterfall_h, LV_COLOR_FORMAT_NATIVE);
        lv_canvas_fill_bg(s_spectrum, lv_color_hex(0x000000), LV_OPA_COVER);
        bsp_display_unlock();
    }
    bsp_display_backlight_on();

    ESP_LOGI(TAG, "ESP32-P4 BSP LCD/touch ready: %dx%d", s_lcd_w, s_lcd_h);
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
    if (!s_spectrum || !s_canvas_buf) {
        return;
    }

    const int tx_role = state->display_role != 0;
    s_band_min_hz = tx_role ? 100 : modem_mode_band_min_hz(state->selected_mode);
    s_band_max_hz = tx_role ? 3500 : modem_mode_band_max_hz(state->selected_mode);

    if (bsp_display_lock(0)) {
        waterfall_render_row(s_canvas_buf, spectrum);
        lv_obj_invalidate(s_spectrum);
        update_tone_marker(tx_role ? state->tx_tone_hz : state->rx_tone_hz);

        char status[128];
        const char *mode_name = tx_role ?
                                psk_mode_name(state->selected_tx_psk_mode) :
                                (demod_profile_for_mode(state->selected_mode)->family == DEMOD_FAMILY_PSK ?
                                 psk_mode_name(state->selected_psk_mode) :
                                 modem_mode_name(state->selected_mode));
        snprintf(status, sizeof(status), "%s %s %s %d Hz Peak %d Hz TX %d/%u",
                 mode_name, transport_mode_name(state->transport_mode),
                 tx_role ? "TX" : "RX",
                 tx_role ? state->tx_tone_hz : state->rx_tone_hz,
                 spectrum->peak_hz, state->tx_active, (unsigned)state->tx_queued);
        lv_label_set_text(s_status, status);
        lv_label_set_text(s_text, tx_role ? state->tx_text : state->text);
        lv_obj_update_layout(s_text_view);
        lv_obj_scroll_to_y(s_text_view, LV_COORD_MAX, LV_ANIM_OFF);
        bsp_display_unlock();
    }
}

int display_take_tone_selection(int *tone_hz)
{
    if (!s_has_pending_tone) {
        return 0;
    }
    *tone_hz = s_pending_tone_hz;
    s_has_pending_tone = 0;
    return 1;
}

void display_task_handler(void)
{
}
