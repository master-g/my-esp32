#ifndef BSP_DISPLAY_H
#define BSP_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "core_types/lvgl_forward.h"
#include "esp_err.h"

typedef void (*bsp_display_ui_cb_t)(void);

typedef struct {
    uint16_t flush_ms_x10;
    uint16_t rotate_ms_x10;
    uint16_t wait_ms_x10;
    uint16_t push_ms_x10;
} bsp_display_perf_snapshot_t;

typedef struct {
    uint32_t wait_us;
    uint32_t push_us;
} bsp_display_push_stats_t;

esp_err_t bsp_display_init(void);
bool bsp_display_lock(uint32_t timeout_ms);
void bsp_display_unlock(void);
lv_obj_t *bsp_display_get_app_root(void);
void bsp_display_set_backlight_percent(uint8_t percent);
void bsp_display_set_ui_callback(bsp_display_ui_cb_t cb);
// 当前任务是否为 LVGL 端口任务。用于断言"只能在 LVGL 任务调用"的约定。
bool bsp_display_is_in_lvgl_task(void);
void bsp_display_get_perf_snapshot(bsp_display_perf_snapshot_t *out);
bool bsp_display_begin_direct_mode(void);
void bsp_display_end_direct_mode(void);
esp_err_t bsp_display_push_native_rgb565(const uint16_t *pixels, uint16_t rows, uint16_t y_offset,
                                         bsp_display_push_stats_t *stats);
// 启动失败时画一块纯色错误屏(独占拿锁,显示未就绪则返回 false)。错误码由调用方打串口。
bool bsp_display_show_fatal_screen(void);

#endif
