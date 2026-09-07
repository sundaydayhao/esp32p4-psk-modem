/* Render and exercise the actual production LVGL UI, with only the BSP mocked. */
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include "../../main/display.c"

static uint16_t framebuffer[1024 * 600], draw_buffer[1024 * 50];
static display_state_t state;
static spectrum_t spectrum;

static content_status_t mock_status;
void content_get_status(content_status_t *out) { *out = mock_status; }
void content_copy_image(uint8_t *pixels, content_status_t *out) {
    *out = mock_status;
    memcpy(pixels, g_modem_test_image_rgb332, MODEM_IMAGE_PIXEL_COUNT);
    if (mock_status.image_rows < 96) memset(pixels + mock_status.image_rows * 96, 0, (96-mock_status.image_rows)*96);
}

static void flush(lv_display_t *disp, const lv_area_t *area, uint8_t *pixels)
{
    const unsigned width = area->x2 - area->x1 + 1;
    for (int y = area->y1; y <= area->y2; ++y)
        memcpy(framebuffer + y * 1024 + area->x1,
               pixels + (y - area->y1) * width * 2, width * 2);
    lv_display_flush_ready(disp);
}

lv_display_t *bsp_display_start_with_config(const bsp_display_cfg_t *config)
{
    (void)config;
    lv_init();
    lv_display_t *disp = lv_display_create(1024, 600);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush);
    return disp;
}

static void pump(void)
{
    for (int i = 0; i < 4; ++i) { lv_tick_inc(40); lv_timer_handler(); }
    lv_refr_now(NULL);
}

static void screenshot(const char *name)
{
    pump();
    FILE *f = fopen(name, "wb");
    assert(f);
    fprintf(f, "P6\n1024 600\n255\n");
    for (unsigned i = 0; i < 1024 * 600; ++i) {
        const uint16_t px = framebuffer[i];
        const uint8_t rgb[] = {(px >> 11) * 255 / 31, ((px >> 5) & 63) * 255 / 63, (px & 31) * 255 / 31};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static void select_option(lv_obj_t *dropdown, unsigned index)
{
    lv_dropdown_close(dropdown);
    lv_dropdown_set_selected(dropdown, index);
    /* Match the LVGL pointer-selection event order. */
    lv_obj_send_event(dropdown, LV_EVENT_CANCEL, NULL);
    lv_obj_send_event(dropdown, LV_EVENT_VALUE_CHANGED, NULL);
    pump();
}

int main(void)
{
    /* Verify the added 3-4 kHz span contains real FFT data, not just new labels. */
    int16_t samples[SPECTRUM_BLOCK_SAMPLES];
    for (unsigned block = 0; block < SPECTRUM_FFT_SIZE / SPECTRUM_BLOCK_SAMPLES; ++block) {
        for (unsigned i = 0; i < SPECTRUM_BLOCK_SAMPLES; ++i)
            samples[i] = (int16_t)(16000 * sin(2.0 * 3.141592653589793 * 3500 *
                (block * SPECTRUM_BLOCK_SAMPLES + i) / SPECTRUM_SAMPLE_RATE_HZ));
        spectrum_analyze(samples, SPECTRUM_BLOCK_SAMPLES, SPECTRUM_SAMPLE_RATE_HZ, &spectrum);
    }
    assert(abs(spectrum.peak_hz - 3500) <= 2 && spectrum.bins[3500] > 0);
    printf("PASS: extended spectrum detects 3500 Hz (peak %d Hz)\n", spectrum.peak_hz);
    assert(display_init() == ESP_OK);
    state.rx_tone_hz = state.tx_tone_hz = 2000;
    spectrum.peak_hz = 2000;
    spectrum.peak_dbfs = -18;
    display_render(&state, &spectrum);
    screenshot("lcd-idle.ppm");

    state.selected_psk_mode = PSK_MODE_2X8PSK1200F;
    display_append_text(&state, "CQ CQ CQ DE HPM6750\nESP32-P4 PSK receiver\n\n2x8PSK1200F / 2000 Hz\n\nTHE QUICK BROWN FOX\n0123456789\n\n[PREVIEW SAMPLE TEXT]");
    for (unsigned row = 0; row < 375; ++row) {
        for (unsigned hz = 0; hz < SPECTRUM_BINS; ++hz) {
            const double d1 = (hz - 1138.0) / 155.0, d2 = (hz - 2862.0) / 155.0;
            spectrum.bins[hz] = 8 + (hz * 7 + row * 13) % 15 +
                (unsigned)(175 * (exp(-d1*d1) + exp(-d2*d2)));
        }
        state.update_waterfall = 1;
        display_render(&state, &spectrum);
    }
    state.update_waterfall = 0;
    screenshot("lcd-receive.ppm");
    lv_dropdown_open(s_mode_dropdown);
    screenshot("lcd-families.ppm");
    select_option(s_mode_dropdown, PSK_MODE_FAMILY_8PSK);
    screenshot("lcd-8psk-modes.ppm");
    /* Periodic state updates must preserve the child list selection. */
    const unsigned before = lv_dropdown_get_selected(s_mode_dropdown);
    display_render(&state, &spectrum);
    assert(lv_dropdown_get_selected(s_mode_dropdown) == before);
    lv_dropdown_close(s_mode_dropdown);
    lv_obj_send_event(s_mode_dropdown, LV_EVENT_CANCEL, NULL);
    pump();
    assert(s_mode_level == 0);
    assert(strcmp(lv_dropdown_get_text(s_mode_dropdown), "2x8PSK1200F") == 0);

    /* Every supported mode, including the previously clipped 28th button. */
    unsigned tested = 0;
    for (unsigned family = 0; family < 3; ++family) {
        for (size_t i = 0; i < psk_mode_count(); ++i) {
            const psk_mode_info_t *info = psk_mode_info_at(i);
            if ((unsigned)info->family != family) continue;
            select_option(s_mode_dropdown, family);
            unsigned index = 0;
            while (index < s_child_count && s_child_modes[index] != info->id) ++index;
            assert(index < s_child_count);
            select_option(s_mode_dropdown, index);
            psk_mode_t selected;
            assert(display_take_mode_selection(&selected) && selected == info->id);
            assert(!display_take_mode_selection(&selected));
            state.selected_psk_mode = selected;
            display_render(&state, &spectrum);
            ++tested;
        }
    }
    select_option(s_role_dropdown, 1);
    int role;
    assert(display_take_role_selection(&role) && role == 1);
    state.display_role = role;
    snprintf(state.tx_text, sizeof(state.tx_text), "TX PREVIEW\nCQ CQ CQ DE ESP32-P4\n2000 Hz");
    display_render(&state, &spectrum);
    screenshot("lcd-transmit.ppm");
    select_option(s_content_dropdown, 1);
    content_type_t selected_content;
    assert(display_take_content_selection(&selected_content) && selected_content == CONTENT_IMAGE);
    state.content_type = selected_content;
    mock_status.image_rows = 96; mock_status.image_version = 1;
    display_render(&state, &spectrum);
    screenshot("lcd-image-tx.ppm");
    state.display_role = 0; mock_status.image_rows = 63; ++mock_status.image_version;
    display_render(&state, &spectrum);
    screenshot("lcd-image-rx.ppm");
    assert(s_family_count == 2);
    select_option(s_mode_dropdown, 1);
    for (unsigned i = 0; i < s_child_count; ++i) assert(modem_image_mode_allowed(s_child_modes[i]));
    lv_dropdown_close(s_mode_dropdown);
    select_option(s_content_dropdown, 2);
    assert(display_take_content_selection(&selected_content) && selected_content == CONTENT_AUDIO);
    state.content_type = selected_content; mock_status.voice_rate = 600;
    mock_status.voice_packets = 24; mock_status.voice_frames = 144;
    display_render(&state, &spectrum);
    assert(lv_obj_has_state(s_mode_dropdown, LV_STATE_DISABLED));
    screenshot("lcd-audio.ppm");
    printf("PASS: %u modes; child cancellation; render while open; Rx/Tx; Image progress; Audio status\n", tested);
    return 0;
}
