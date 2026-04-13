#ifndef SERVICE_MARKET_STREAM_BINANCE_H
#define SERVICE_MARKET_STREAM_BINANCE_H

#include <stdbool.h>

#include "esp_err.h"
#include "market_feed.h"

typedef struct {
    void (*on_connected)(market_pair_id_t pair, market_interval_id_t interval, void *context);
    void (*on_disconnected)(market_pair_id_t pair, market_interval_id_t interval,
                            esp_err_t last_esp_err, int status_code, void *context);
    void (*on_summary)(market_pair_id_t pair, const market_feed_summary_t *summary, void *context);
    void (*on_kline)(market_pair_id_t pair, market_interval_id_t interval,
                     const market_candle_t *candle, void *context);
} market_binance_stream_callbacks_t;

/* callback context must remain valid until market_binance_stream_deinit() returns */
esp_err_t market_binance_stream_init(const market_binance_stream_callbacks_t *callbacks,
                                     void *context);
void market_binance_stream_deinit(void);
esp_err_t market_binance_stream_start(market_pair_id_t pair, market_interval_id_t interval);
void market_binance_stream_stop(void);
bool market_binance_stream_is_connected(void);

#endif
