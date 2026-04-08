#ifndef BSP_BOARD_CONFIG_H
#define BSP_BOARD_CONFIG_H

#include "driver/gpio.h"
#include "driver/i2c_types.h"
#include "driver/spi_master.h"

#define BSP_BOARD_NAME "Waveshare ESP32-S3-Touch-LCD-3.49"

/*
 * Baseline chosen from the official Waveshare ESP-IDF examples on GitHub
 * as of April 8, 2026. The public product page contains conflicting text
 * about touch type and resolution; the implementation baseline follows the
 * maintained example code instead:
 * - AXS15231B QSPI LCD
 * - Physical panel: 172x640 portrait
 * - Runtime UI: rotated to 640x172 landscape
 * - Dedicated touch I2C bus at address 0x3B
 * - RTC at 0x51 and QMI8658 IMU at 0x6B
 */
#define BSP_LCD_PANEL_H_RES 172
#define BSP_LCD_PANEL_V_RES 640

#define BSP_LCD_H_RES BSP_LCD_PANEL_V_RES
#define BSP_LCD_V_RES BSP_LCD_PANEL_H_RES

#define BSP_UI_ROTATION_LANDSCAPE_90 90
#define BSP_UI_ROTATION_LANDSCAPE_270 270

/*
 * The runtime UI supports two fixed landscape install directions.
 * 90  = current landscape
 * 270 = upside-down landscape relative to the 90-degree orientation
 */
#define BSP_UI_ROTATION BSP_UI_ROTATION_LANDSCAPE_270

#define BSP_LCD_HOST SPI3_HOST
#define BSP_LCD_CS GPIO_NUM_9
#define BSP_LCD_PCLK GPIO_NUM_10
#define BSP_LCD_DATA0 GPIO_NUM_11
#define BSP_LCD_DATA1 GPIO_NUM_12
#define BSP_LCD_DATA2 GPIO_NUM_13
#define BSP_LCD_DATA3 GPIO_NUM_14
#define BSP_LCD_RST GPIO_NUM_21
#define BSP_LCD_BACKLIGHT GPIO_NUM_8

#define BSP_SENSOR_I2C_PORT I2C_NUM_0
#define BSP_SENSOR_I2C_SCL GPIO_NUM_48
#define BSP_SENSOR_I2C_SDA GPIO_NUM_47

#define BSP_TOUCH_I2C_PORT I2C_NUM_1
#define BSP_TOUCH_I2C_SCL GPIO_NUM_18
#define BSP_TOUCH_I2C_SDA GPIO_NUM_17

#define BSP_TOUCH_I2C_ADDR 0x3B
#define BSP_RTC_I2C_ADDR 0x51
#define BSP_IMU_I2C_ADDR 0x6B

#endif
