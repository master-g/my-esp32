#ifndef SERVICE_MARKET_SYMBOLS_H
#define SERVICE_MARKET_SYMBOLS_H

#include <stdbool.h>

#include "service_market.h"

typedef struct {
    const char *symbol;
    bool invert_price;
} market_symbol_config_t;

bool market_source_pair_config(market_source_t source, market_pair_id_t pair,
                               market_symbol_config_t *out);
const char *market_interval_symbol(market_interval_id_t interval);

#endif
