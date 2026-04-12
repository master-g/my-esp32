#ifndef APP_TRADING_PRESENTER_H
#define APP_TRADING_PRESENTER_H

#include <stdbool.h>
#include <stdint.h>

#include "service_market.h"

typedef struct {
    char price_text[24];
    char change_text[16];
    char status_text[48];
    char chart_status_text[48];
    char transport_text[16];
    uint32_t price_color;
    uint32_t change_color;
    uint32_t status_color;
    uint32_t transport_color;
    bool pair_selected[MARKET_PAIR_COUNT];
    bool interval_selected[MARKET_INTERVAL_COUNT];
    bool has_chart_data;
    bool chart_dimmed;
    bool fallback_active;
    market_candle_window_t candles;
} trading_present_model_t;

void trading_presenter_build(trading_present_model_t *out, const market_snapshot_t *snapshot,
                             const market_candle_window_t *candles);

#endif
