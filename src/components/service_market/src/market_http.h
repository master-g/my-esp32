#ifndef SERVICE_MARKET_HTTP_H
#define SERVICE_MARKET_HTTP_H

#include <stddef.h>

#include "esp_err.h"

esp_err_t market_http_get_json(const char *url, size_t capacity, char **out_body);

#endif
