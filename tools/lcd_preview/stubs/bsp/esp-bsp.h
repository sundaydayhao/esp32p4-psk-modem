#pragma once
#include <stdbool.h>
#include "lvgl.h"
#define BSP_LCD_H_RES 1024
#define BSP_LCD_V_RES 600
#define BSP_LCD_DRAW_BUFF_SIZE (1024 * 50)
#define BSP_LCD_DRAW_BUFF_DOUBLE 0
#define BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS 1000
#define BSP_HDMI_RES_NONE 0
#define ESP_LVGL_PORT_INIT_CONFIG() {0}
typedef struct {
    struct { int task_affinity; } lvgl_port_cfg;
    unsigned buffer_size;
    bool double_buffer;
    struct {
        int hdmi_resolution;
        struct { int phy_clk_src, lane_bit_rate_mbps; } dsi_bus;
    } hw_cfg;
    struct { bool buff_dma, buff_spiram, sw_rotate; } flags;
} bsp_display_cfg_t;
lv_display_t *bsp_display_start_with_config(const bsp_display_cfg_t *config);
static inline bool bsp_display_lock(unsigned timeout) { (void)timeout; return true; }
static inline void bsp_display_unlock(void) {}
static inline void bsp_display_backlight_on(void) {}
