#ifndef BSP_DISPLAY_H
#define BSP_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "core_types/lvgl_forward.h"
#include "esp_err.h"

esp_err_t bsp_display_init(void);
bool bsp_display_lock(uint32_t timeout_ms);
void bsp_display_unlock(void);
lv_obj_t *bsp_display_get_app_root(void);
void bsp_display_set_backlight_percent(uint8_t percent);

#endif
