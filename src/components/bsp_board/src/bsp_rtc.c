#include "bsp_rtc.h"

#include <string.h>
#include <time.h>

#include "bsp_board_config.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"

#define PCF85063_REG_CTRL1 0x00
#define PCF85063_REG_SECONDS 0x04
#define PCF85063_REG_STOP_MASK (1U << 5)
#define PCF85063_REG_SECONDS_VALID_MASK (1U << 7)

static i2c_master_bus_handle_t s_sensor_bus;
static i2c_master_dev_handle_t s_rtc_dev;
static bool s_initialized;

static uint8_t bcd_to_dec(uint8_t value)
{
    return (uint8_t)(((value >> 4) * 10U) + (value & 0x0FU));
}

static uint8_t dec_to_bcd(uint8_t value) { return (uint8_t)(((value / 10U) << 4) | (value % 10U)); }

static esp_err_t read_registers(uint8_t start_reg, uint8_t *buffer, size_t len)
{
    return i2c_master_transmit_receive(s_rtc_dev, &start_reg, 1, buffer, len, pdMS_TO_TICKS(100));
}

static esp_err_t write_registers(uint8_t start_reg, const uint8_t *buffer, size_t len)
{
    uint8_t temp[8];

    ESP_RETURN_ON_FALSE(len < sizeof(temp), ESP_ERR_INVALID_SIZE, "bsp_rtc", "rtc write too large");
    temp[0] = start_reg;
    memcpy(&temp[1], buffer, len);
    return i2c_master_transmit(s_rtc_dev, temp, len + 1, pdMS_TO_TICKS(100));
}

bool bsp_rtc_is_time_plausible(uint32_t epoch_s)
{
    struct tm tm_value;
    time_t raw = (time_t)epoch_s;

    if (epoch_s < 1700000000U) {
        return false;
    }

    memset(&tm_value, 0, sizeof(tm_value));
    localtime_r(&raw, &tm_value);
    return (tm_value.tm_year + 1900) >= 2024 && (tm_value.tm_year + 1900) <= 2099;
}

esp_err_t bsp_rtc_init(void)
{
    uint8_t ctrl1 = 0;
    esp_err_t err;
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = BSP_SENSOR_I2C_PORT,
        .scl_io_num = BSP_SENSOR_I2C_SCL,
        .sda_io_num = BSP_SENSOR_I2C_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    if (s_initialized) {
        return ESP_OK;
    }

    err = i2c_new_master_bus(&bus_config, &s_sensor_bus);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(i2c_master_get_bus_handle(BSP_SENSOR_I2C_PORT, &s_sensor_bus),
                            "bsp_rtc", "sensor bus handle failed");
    } else if (err != ESP_OK) {
        return err;
    }

    {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = BSP_RTC_I2C_ADDR,
            .scl_speed_hz = 100000,
        };
        ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_sensor_bus, &dev_cfg, &s_rtc_dev),
                            "bsp_rtc", "rtc add device failed");
    }

    ESP_RETURN_ON_ERROR(read_registers(PCF85063_REG_CTRL1, &ctrl1, 1), "bsp_rtc",
                        "rtc ctrl read failed");
    if (ctrl1 & PCF85063_REG_STOP_MASK) {
        ctrl1 &= (uint8_t)~PCF85063_REG_STOP_MASK;
        ESP_RETURN_ON_ERROR(write_registers(PCF85063_REG_CTRL1, &ctrl1, 1), "bsp_rtc",
                            "rtc ctrl write failed");
    }

    s_initialized = true;
    return ESP_OK;
}

esp_err_t bsp_rtc_read_epoch(uint32_t *epoch_s)
{
    uint8_t regs[7] = {0};
    struct tm tm_value = {0};
    time_t utc_time;

    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, "bsp_rtc", "rtc not initialized");
    ESP_RETURN_ON_FALSE(epoch_s != NULL, ESP_ERR_INVALID_ARG, "bsp_rtc", "epoch pointer required");
    ESP_RETURN_ON_ERROR(read_registers(PCF85063_REG_SECONDS, regs, sizeof(regs)), "bsp_rtc",
                        "rtc read failed");
    ESP_RETURN_ON_FALSE((regs[0] & PCF85063_REG_SECONDS_VALID_MASK) == 0, ESP_ERR_INVALID_RESPONSE,
                        "bsp_rtc", "rtc clock integrity not guaranteed");

    tm_value.tm_sec = bcd_to_dec(regs[0] & 0x7FU);
    tm_value.tm_min = bcd_to_dec(regs[1] & 0x7FU);
    tm_value.tm_hour = bcd_to_dec(regs[2] & 0x3FU);
    tm_value.tm_mday = bcd_to_dec(regs[3] & 0x3FU);
    tm_value.tm_wday = bcd_to_dec(regs[4] & 0x07U);
    tm_value.tm_mon = bcd_to_dec(regs[5] & 0x1FU) - 1;
    tm_value.tm_year = bcd_to_dec(regs[6]) + 100;
    tm_value.tm_isdst = -1;

    utc_time = timegm(&tm_value);
    ESP_RETURN_ON_FALSE(utc_time > 0, ESP_ERR_INVALID_RESPONSE, "bsp_rtc",
                        "rtc returned invalid utc time");
    ESP_RETURN_ON_FALSE(bsp_rtc_is_time_plausible((uint32_t)utc_time), ESP_ERR_INVALID_RESPONSE,
                        "bsp_rtc", "rtc time not plausible");

    *epoch_s = (uint32_t)utc_time;
    return ESP_OK;
}

esp_err_t bsp_rtc_write_epoch(uint32_t epoch_s)
{
    struct tm tm_value;
    time_t raw = (time_t)epoch_s;
    uint8_t regs[7];

    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, "bsp_rtc", "rtc not initialized");
    gmtime_r(&raw, &tm_value);

    regs[0] = dec_to_bcd((uint8_t)tm_value.tm_sec) & 0x7FU;
    regs[1] = dec_to_bcd((uint8_t)tm_value.tm_min);
    regs[2] = dec_to_bcd((uint8_t)tm_value.tm_hour);
    regs[3] = dec_to_bcd((uint8_t)tm_value.tm_mday);
    regs[4] = (uint8_t)(tm_value.tm_wday & 0x07U);
    regs[5] = dec_to_bcd((uint8_t)(tm_value.tm_mon + 1));
    regs[6] = dec_to_bcd((uint8_t)((tm_value.tm_year + 1900) % 100));

    return write_registers(PCF85063_REG_SECONDS, regs, sizeof(regs));
}
