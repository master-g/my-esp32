#ifndef WEATHER_CLIENT_H
#define WEATHER_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "service_weather.h"

typedef struct {
    int16_t temperature_c_tenths;
    uint16_t weather_code;
    bool is_day;
} weather_client_result_t;

esp_err_t weather_client_fetch_current(const weather_location_config_t *location,
                                       weather_client_result_t *result);

#endif
