#include "bsp_board.h"

#include <string.h>

#include "bsp_board_config.h"
#include "bsp_display.h"
#include "bsp_rtc.h"
#include "bsp_touch.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "bsp_board";
static bsp_board_status_t s_status;

esp_err_t bsp_board_init(void)
{
    memset(&s_status, 0, sizeof(s_status));
    ESP_LOGI(TAG,
             "Board baseline pinned: panel=%ux%u, ui=%ux%u landscape, touch=%d/%d@0x%02x, sensor=%d/%d, rtc=0x%02x, imu=0x%02x",
             BSP_LCD_PANEL_H_RES,
             BSP_LCD_PANEL_V_RES,
             BSP_LCD_H_RES,
             BSP_LCD_V_RES,
             BSP_TOUCH_I2C_SCL,
             BSP_TOUCH_I2C_SDA,
             BSP_TOUCH_I2C_ADDR,
             BSP_SENSOR_I2C_SCL,
             BSP_SENSOR_I2C_SDA,
             BSP_RTC_I2C_ADDR,
             BSP_IMU_I2C_ADDR);

    ESP_RETURN_ON_ERROR(bsp_touch_init(), TAG, "touch init failed");
    s_status.touch_ready = true;

    ESP_RETURN_ON_ERROR(bsp_rtc_init(), TAG, "rtc init failed");
    s_status.rtc_ready = true;

    ESP_RETURN_ON_ERROR(bsp_display_init(), TAG, "display init failed");
    s_status.display_ready = true;
    s_status.backlight_ready = true;

    return ESP_OK;
}

const bsp_board_status_t *bsp_board_get_status(void)
{
    return &s_status;
}

bool bsp_board_lock(uint32_t timeout_ms)
{
    return bsp_display_lock(timeout_ms);
}

void bsp_board_unlock(void)
{
    bsp_display_unlock();
}

lv_obj_t *bsp_board_get_app_root(void)
{
    return bsp_display_get_app_root();
}

void bsp_board_set_backlight_percent(uint8_t percent)
{
    bsp_display_set_backlight_percent(percent);
}
