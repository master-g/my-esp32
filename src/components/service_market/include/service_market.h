#ifndef SERVICE_MARKET_H
#define SERVICE_MARKET_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define MARKET_PAIR_COUNT 3
#define MARKET_INTERVAL_COUNT 3
#define MARKET_MAX_CANDLES 32

typedef enum {
    MARKET_PAIR_BTC_USDT = 0,
    MARKET_PAIR_ETH_USDT,
    MARKET_PAIR_BTC_ETH,
} market_pair_id_t;

typedef enum {
    MARKET_INTERVAL_1H = 0,
    MARKET_INTERVAL_4H,
    MARKET_INTERVAL_1D,
} market_interval_id_t;

typedef enum {
    TRADING_DATA_EMPTY = 0,
    TRADING_DATA_LOADING,
    TRADING_DATA_LIVE,
    TRADING_DATA_STALE,
    TRADING_DATA_ERROR,
} trading_data_state_t;

typedef enum {
    MARKET_TRANSPORT_POLLING = 0,
    MARKET_TRANSPORT_STREAMING,
} market_transport_hint_t;

typedef enum {
    MARKET_SOURCE_OKX = 0,
    MARKET_SOURCE_BINANCE,
    MARKET_SOURCE_COUNT,
    MARKET_SOURCE_UNKNOWN = 0xff,
} market_source_t;

typedef struct {
    market_pair_id_t pair;
    market_interval_id_t interval;
} market_selection_t;

typedef struct {
    uint32_t open_time_epoch_s;
    int32_t open_scaled;
    int32_t high_scaled;
    int32_t low_scaled;
    int32_t close_scaled;
    uint32_t volume_scaled;
} market_candle_t;

typedef struct {
    uint16_t count;
    market_candle_t candles[MARKET_MAX_CANDLES];
} market_candle_window_t;

typedef struct {
    market_selection_t selection;
    trading_data_state_t state;
    bool wifi_connected;
    uint32_t summary_updated_at_epoch_s;
    uint32_t chart_updated_at_epoch_s;
    char pair_label[16];
    char price_text[24];
    char change_text[16];
    int32_t change_bp;
    int32_t last_price_scaled;
    bool has_chart_data;
    uint16_t candle_count;
    market_transport_hint_t transport_hint;
    market_source_t active_source;
    bool fallback_active;
    uint8_t source_error_count;
} market_snapshot_t;

const char *market_pair_label(market_pair_id_t pair);
const char *market_interval_label(market_interval_id_t interval);
const char *market_source_label(market_source_t source);

esp_err_t market_service_init(void);
void market_service_select_pair(market_pair_id_t pair);
void market_service_select_interval(market_interval_id_t interval);
void market_service_get_snapshot(market_snapshot_t *out);
bool market_service_has_chart_data(market_pair_id_t pair, market_interval_id_t interval);
bool market_service_get_candles(market_pair_id_t pair, market_interval_id_t interval,
                                market_candle_window_t *out);

#endif
