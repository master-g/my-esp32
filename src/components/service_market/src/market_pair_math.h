#ifndef SERVICE_MARKET_PAIR_MATH_H
#define SERVICE_MARKET_PAIR_MATH_H

#include "esp_err.h"

static inline esp_err_t market_invert_price_value(double value, double *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (value <= 0.0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out = 1.0 / value;
    return ESP_OK;
}

static inline esp_err_t market_invert_change_percent(double value, double *out)
{
    const double change_ratio = value / 100.0;

    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (change_ratio <= -0.999999) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out = ((1.0 / (1.0 + change_ratio)) - 1.0) * 100.0;
    return ESP_OK;
}

#endif
