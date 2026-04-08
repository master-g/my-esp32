#include "bsp_display.h"

#include <stdint.h>

#include "bsp_board_config.h"
#include "driver/ledc.h"
#include "esp_check.h"

#define BSP_BACKLIGHT_LEDC_MODE LEDC_LOW_SPEED_MODE
#define BSP_BACKLIGHT_LEDC_TIMER LEDC_TIMER_3
#define BSP_BACKLIGHT_LEDC_CHANNEL LEDC_CHANNEL_1

static bool s_backlight_ready;

static uint32_t backlight_percent_to_duty(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }

    return (uint32_t)(((100U - percent) * 255U) / 100U);
}

esp_err_t bsp_backlight_init(void)
{
    ledc_timer_config_t timer_conf = {
        .speed_mode = BSP_BACKLIGHT_LEDC_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = BSP_BACKLIGHT_LEDC_TIMER,
        .freq_hz = 50 * 1000,
        .clk_cfg = LEDC_SLOW_CLK_RC_FAST,
    };
    ledc_channel_config_t channel_conf = {
        .gpio_num = BSP_LCD_BACKLIGHT,
        .speed_mode = BSP_BACKLIGHT_LEDC_MODE,
        .channel = BSP_BACKLIGHT_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BSP_BACKLIGHT_LEDC_TIMER,
        .duty = backlight_percent_to_duty(0),
        .hpoint = 0,
    };

    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_conf), "bsp_backlight", "timer init failed");
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel_conf), "bsp_backlight", "channel init failed");
    s_backlight_ready = true;
    return ESP_OK;
}

void bsp_backlight_set_percent(uint8_t percent)
{
    if (!s_backlight_ready) {
        return;
    }

    ledc_set_duty(BSP_BACKLIGHT_LEDC_MODE, BSP_BACKLIGHT_LEDC_CHANNEL, backlight_percent_to_duty(percent));
    ledc_update_duty(BSP_BACKLIGHT_LEDC_MODE, BSP_BACKLIGHT_LEDC_CHANNEL);
}
