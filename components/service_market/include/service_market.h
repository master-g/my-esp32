#ifndef SERVICE_MARKET_H
#define SERVICE_MARKET_H

#include <stdbool.h>
#include <stdint.h>

#include "core_types/power_policy_types.h"
#include "esp_err.h"

typedef enum {
    MARKET_PAIR_BTC_USDT = 0,
    MARKET_PAIR_ETH_USDT,
    MARKET_PAIR_BTC_ETH,
} market_pair_id_t;

typedef enum {
    MARKET_INTERVAL_1M = 0,
    MARKET_INTERVAL_5M,
    MARKET_INTERVAL_1H,
    MARKET_INTERVAL_1D,
} market_interval_id_t;

typedef enum {
    TRADING_DATA_EMPTY = 0,
    TRADING_DATA_LIVE,
    TRADING_DATA_STALE,
    TRADING_DATA_ERROR,
} trading_data_state_t;

typedef struct {
    market_pair_id_t pair;
    market_interval_id_t interval;
} market_selection_t;

typedef struct {
    uint32_t open_time_epoch_s;
    float open;
    float high;
    float low;
    float close;
    float volume;
} market_candle_t;

typedef struct {
    market_selection_t selection;
    trading_data_state_t state;
    bool wifi_connected;
    uint32_t updated_at_epoch_s;
    char pair_label[16];
    char price_text[24];
    char change_text[16];
    int32_t change_bp;
    bool has_chart_data;
} market_snapshot_t;

esp_err_t market_service_init(void);
void market_service_select_pair(market_pair_id_t pair);
void market_service_select_interval(market_interval_id_t interval);
void market_service_on_refresh_mode_changed(refresh_mode_t mode);
const market_snapshot_t *market_service_get_snapshot(void);
const market_candle_t *market_service_get_candles(market_pair_id_t pair,
                                                  market_interval_id_t interval,
                                                  uint16_t *count);

#endif
