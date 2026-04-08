#ifndef BSP_BOARD_H
#define BSP_BOARD_H

#include <stdbool.h>
#include <stdint.h>

#include "core_types/lvgl_forward.h"
#include "esp_err.h"

typedef struct {
    bool display_ready;
    bool touch_ready;
    bool rtc_ready;
    bool imu_ready;
    bool backlight_ready;
} bsp_board_status_t;

esp_err_t bsp_board_init(void);
const bsp_board_status_t *bsp_board_get_status(void);
bool bsp_board_lock(uint32_t timeout_ms);
void bsp_board_unlock(void);
lv_obj_t *bsp_board_get_app_root(void);
void bsp_board_set_backlight_percent(uint8_t percent);

#endif
