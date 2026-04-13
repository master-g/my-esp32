#ifndef SERVICE_MARKET_FEED_H
#define SERVICE_MARKET_FEED_H

#include "service_market.h"

typedef struct {
    int32_t last_price_scaled;
    int32_t change_bp;
} market_feed_summary_t;

typedef struct {
    market_source_t source;
    const char *source_label;
    market_transport_hint_t (*transport_hint)(void);
    esp_err_t (*fetch_summary)(market_pair_id_t pair, market_feed_summary_t *out);
    esp_err_t (*fetch_candles)(market_pair_id_t pair, market_interval_id_t interval,
                               market_candle_window_t *out);
} market_feed_iface_t;

const market_feed_iface_t *market_feed_binance_get_iface(void);
const market_feed_iface_t *market_feed_gate_get_iface(void);

#endif
