#ifndef SERVICE_MARKET_HTTP_H
#define SERVICE_MARKET_HTTP_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t market_http_get_json(const char *url, size_t capacity, char **out_body);

int32_t market_scale_double(double value, uint32_t factor);
uint32_t market_scale_positive_double(double value, uint32_t factor);

#endif
