#include "bsp_touch.h"

#include <string.h>

#include "bsp_board_config.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"

static i2c_master_bus_handle_t s_touch_bus;
static i2c_master_dev_handle_t s_touch_dev;
static bool s_touch_ready;

esp_err_t bsp_touch_init(void)
{
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = BSP_TOUCH_I2C_PORT,
        .scl_io_num = BSP_TOUCH_I2C_SCL,
        .sda_io_num = BSP_TOUCH_I2C_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BSP_TOUCH_I2C_ADDR,
        .scl_speed_hz = 300000,
    };

    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &s_touch_bus), "bsp_touch", "touch bus init failed");
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_touch_bus, &dev_cfg, &s_touch_dev), "bsp_touch", "touch dev add failed");
    s_touch_ready = true;
    return ESP_OK;
}

esp_err_t bsp_touch_read(bsp_touch_point_t *point)
{
    static const uint8_t cmd[11] = {0xb5, 0xab, 0xa5, 0x5a, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00};
    uint8_t buf[32] = {0};
    uint16_t raw_x = 0;
    uint16_t raw_y = 0;

    ESP_RETURN_ON_FALSE(s_touch_ready, ESP_ERR_INVALID_STATE, "bsp_touch", "touch not initialized");
    ESP_RETURN_ON_FALSE(point != NULL, ESP_ERR_INVALID_ARG, "bsp_touch", "point is required");

    memset(point, 0, sizeof(*point));
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(s_touch_dev,
                                                    cmd,
                                                    sizeof(cmd),
                                                    buf,
                                                    sizeof(buf),
                                                    pdMS_TO_TICKS(100)),
                        "bsp_touch",
                        "touch read failed");

    if (buf[1] == 0 || buf[1] >= 5) {
        point->pressed = false;
        return ESP_OK;
    }

    raw_x = (uint16_t)(((uint16_t)buf[2] & 0x0fU) << 8) | (uint16_t)buf[3];
    raw_y = (uint16_t)(((uint16_t)buf[4] & 0x0fU) << 8) | (uint16_t)buf[5];

    if (raw_x > BSP_LCD_PANEL_V_RES) {
        raw_x = BSP_LCD_PANEL_V_RES;
    }
    if (raw_y > BSP_LCD_PANEL_H_RES) {
        raw_y = BSP_LCD_PANEL_H_RES;
    }

    point->pressed = true;
    point->x = raw_y;
    point->y = (uint16_t)(BSP_LCD_PANEL_V_RES - raw_x);
    return ESP_OK;
}
