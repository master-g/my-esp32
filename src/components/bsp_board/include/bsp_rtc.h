#ifndef BSP_RTC_H
#define BSP_RTC_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t bsp_rtc_init(void);
esp_err_t bsp_rtc_read_epoch(uint32_t *epoch_s);
esp_err_t bsp_rtc_write_epoch(uint32_t epoch_s);
bool bsp_rtc_is_time_plausible(uint32_t epoch_s);

#endif
