#include "service_market.h"

#include <stdio.h>
#include <string.h>

static market_snapshot_t s_snapshot;
static refresh_mode_t s_refresh_mode;

static void update_pair_label(market_pair_id_t pair)
{
    const char *label = "BTC/USDT";

    switch (pair) {
    case MARKET_PAIR_ETH_USDT:
        label = "ETH/USDT";
        break;
    case MARKET_PAIR_BTC_ETH:
        label = "BTC/ETH";
        break;
    case MARKET_PAIR_BTC_USDT:
    default:
        label = "BTC/USDT";
        break;
    }

    snprintf(s_snapshot.pair_label, sizeof(s_snapshot.pair_label), "%s", label);
}

esp_err_t market_service_init(void)
{
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.selection.pair = MARKET_PAIR_BTC_USDT;
    s_snapshot.selection.interval = MARKET_INTERVAL_1H;
    s_snapshot.state = TRADING_DATA_EMPTY;
    update_pair_label(s_snapshot.selection.pair);
    snprintf(s_snapshot.price_text, sizeof(s_snapshot.price_text), "%s", "--");
    snprintf(s_snapshot.change_text, sizeof(s_snapshot.change_text), "%s", "--");
    s_refresh_mode = REFRESH_MODE_BACKGROUND_CACHE;
    return ESP_OK;
}

void market_service_select_pair(market_pair_id_t pair)
{
    s_snapshot.selection.pair = pair;
    update_pair_label(pair);
}

void market_service_select_interval(market_interval_id_t interval)
{
    s_snapshot.selection.interval = interval;
}

void market_service_on_refresh_mode_changed(refresh_mode_t mode)
{
    s_refresh_mode = mode;
    (void)s_refresh_mode;
}

const market_snapshot_t *market_service_get_snapshot(void) { return &s_snapshot; }

const market_candle_t *market_service_get_candles(market_pair_id_t pair,
                                                  market_interval_id_t interval, uint16_t *count)
{
    (void)pair;
    (void)interval;
    if (count != NULL) {
        *count = 0;
    }
    return NULL;
}
