#include "market_symbols.h"

bool market_source_pair_config(market_source_t source, market_pair_id_t pair,
                               market_symbol_config_t *out)
{
    if (out == NULL) {
        return false;
    }

    switch (source) {
    case MARKET_SOURCE_GATE:
        switch (pair) {
        case MARKET_PAIR_ETH_USDT:
            *out = (market_symbol_config_t){
                .symbol = "ETH_USDT",
                .invert_price = false,
            };
            return true;
        case MARKET_PAIR_BTC_ETH:
            *out = (market_symbol_config_t){
                .symbol = "ETH_BTC",
                .invert_price = true,
            };
            return true;
        case MARKET_PAIR_BTC_USDT:
        default:
            *out = (market_symbol_config_t){
                .symbol = "BTC_USDT",
                .invert_price = false,
            };
            return true;
        }
    case MARKET_SOURCE_BINANCE:
        switch (pair) {
        case MARKET_PAIR_ETH_USDT:
            *out = (market_symbol_config_t){
                .symbol = "ETHUSDT",
                .invert_price = false,
            };
            return true;
        case MARKET_PAIR_BTC_ETH:
            *out = (market_symbol_config_t){
                .symbol = "ETHBTC",
                .invert_price = true,
            };
            return true;
        case MARKET_PAIR_BTC_USDT:
        default:
            *out = (market_symbol_config_t){
                .symbol = "BTCUSDT",
                .invert_price = false,
            };
            return true;
        }
    default:
        break;
    }

    return false;
}

const char *market_interval_symbol(market_interval_id_t interval)
{
    switch (interval) {
    case MARKET_INTERVAL_4H:
        return "4h";
    case MARKET_INTERVAL_1D:
        return "1d";
    case MARKET_INTERVAL_1H:
    default:
        return "1h";
    }
}
