#ifndef BSP_TOUCH_H
#define BSP_TOUCH_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool pressed;
    uint16_t x;
    uint16_t y;
} bsp_touch_point_t;

esp_err_t bsp_touch_init(void);
esp_err_t bsp_touch_read(bsp_touch_point_t *point);

#endif
