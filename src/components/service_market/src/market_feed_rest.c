#include "market_feed.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "market_http.h"

#define MARKET_BINANCE_BASE_URL "https://api.binance.com"
#define MARKET_BINANCE_URL_LEN 256
#define MARKET_BINANCE_SUMMARY_CAPACITY 4096
#define MARKET_BINANCE_CANDLES_CAPACITY 16384

static int32_t scale_double(double value, uint32_t factor)
{
    double scaled = value * (double)factor;

    if (scaled >= 0.0) {
        return (int32_t)(scaled + 0.5);
    }

    return (int32_t)(scaled - 0.5);
}

static uint32_t scale_positive_double(double value, uint32_t factor)
{
    double scaled = value * (double)factor;

    if (scaled <= 0.0) {
        return 0;
    }

    return (uint32_t)(scaled + 0.5);
}

static const char *market_pair_symbol(market_pair_id_t pair)
{
    switch (pair) {
    case MARKET_PAIR_ETH_USDT:
        return "ETHUSDT";
    case MARKET_PAIR_BTC_ETH:
        return "BTCETH";
    case MARKET_PAIR_BTC_USDT:
    default:
        return "BTCUSDT";
    }
}

static const char *market_interval_symbol(market_interval_id_t interval)
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

static esp_err_t parse_summary_body(const char *body, market_feed_summary_t *out)
{
    cJSON *root = NULL;
    const cJSON *last_price = NULL;
    const cJSON *change_percent = NULL;
    const char *last_price_str = NULL;
    const char *change_percent_str = NULL;

    ESP_RETURN_ON_FALSE(body != NULL, ESP_ERR_INVALID_ARG, "market_feed_binance",
                        "body is required");
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, "market_feed_binance",
                        "summary out is required");

    root = cJSON_Parse(body);
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_INVALID_RESPONSE, "market_feed_binance",
                        "summary JSON parse failed");

    last_price = cJSON_GetObjectItemCaseSensitive(root, "lastPrice");
    change_percent = cJSON_GetObjectItemCaseSensitive(root, "priceChangePercent");
    last_price_str = cJSON_GetStringValue(last_price);
    change_percent_str = cJSON_GetStringValue(change_percent);
    if (last_price_str == NULL || change_percent_str == NULL) {
        cJSON_Delete(root);
        ESP_RETURN_ON_FALSE(false, ESP_ERR_INVALID_RESPONSE, "market_feed_binance",
                            "summary fields missing");
    }

    out->last_price_scaled = scale_double(strtod(last_price_str, NULL), 10000U);
    out->change_bp = scale_double(strtod(change_percent_str, NULL), 100U);

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t parse_candles_body(const char *body, market_candle_window_t *out)
{
    cJSON *root = NULL;
    int count = 0;
    int i = 0;

    ESP_RETURN_ON_FALSE(body != NULL, ESP_ERR_INVALID_ARG, "market_feed_binance",
                        "body is required");
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, "market_feed_binance",
                        "candle out is required");

    root = cJSON_Parse(body);
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_INVALID_RESPONSE, "market_feed_binance",
                        "candle JSON parse failed");
    ESP_RETURN_ON_FALSE(cJSON_IsArray(root), ESP_ERR_INVALID_RESPONSE, "market_feed_binance",
                        "candle root is not an array");

    memset(out, 0, sizeof(*out));
    count = cJSON_GetArraySize(root);
    if (count > MARKET_MAX_CANDLES) {
        count = MARKET_MAX_CANDLES;
    }

    for (i = 0; i < count; ++i) {
        const cJSON *entry = cJSON_GetArrayItem(root, i);
        const cJSON *open_time = NULL;
        const char *open_str = NULL;
        const char *high_str = NULL;
        const char *low_str = NULL;
        const char *close_str = NULL;
        const char *volume_str = NULL;

        if (!cJSON_IsArray(entry) || cJSON_GetArraySize(entry) < 6) {
            cJSON_Delete(root);
            ESP_RETURN_ON_FALSE(false, ESP_ERR_INVALID_RESPONSE, "market_feed_binance",
                                "candle entry shape is invalid");
        }

        open_time = cJSON_GetArrayItem(entry, 0);
        open_str = cJSON_GetStringValue(cJSON_GetArrayItem(entry, 1));
        high_str = cJSON_GetStringValue(cJSON_GetArrayItem(entry, 2));
        low_str = cJSON_GetStringValue(cJSON_GetArrayItem(entry, 3));
        close_str = cJSON_GetStringValue(cJSON_GetArrayItem(entry, 4));
        volume_str = cJSON_GetStringValue(cJSON_GetArrayItem(entry, 5));
        if (!cJSON_IsNumber(open_time) || open_str == NULL || high_str == NULL || low_str == NULL ||
            close_str == NULL || volume_str == NULL) {
            cJSON_Delete(root);
            ESP_RETURN_ON_FALSE(false, ESP_ERR_INVALID_RESPONSE, "market_feed_binance",
                                "candle fields missing");
        }

        out->candles[i].open_time_epoch_s = (uint32_t)(cJSON_GetNumberValue(open_time) / 1000.0);
        out->candles[i].open_scaled = scale_double(strtod(open_str, NULL), 10000U);
        out->candles[i].high_scaled = scale_double(strtod(high_str, NULL), 10000U);
        out->candles[i].low_scaled = scale_double(strtod(low_str, NULL), 10000U);
        out->candles[i].close_scaled = scale_double(strtod(close_str, NULL), 10000U);
        out->candles[i].volume_scaled = scale_positive_double(strtod(volume_str, NULL), 100U);
    }

    out->count = (uint16_t)count;
    cJSON_Delete(root);
    return ESP_OK;
}

static market_transport_hint_t market_feed_binance_transport_hint(void)
{
    return MARKET_TRANSPORT_POLLING;
}

static esp_err_t market_feed_binance_fetch_summary(market_pair_id_t pair,
                                                   market_feed_summary_t *out)
{
    char url[MARKET_BINANCE_URL_LEN];
    char *body = NULL;
    esp_err_t err;

    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, "market_feed_binance",
                        "summary out is required");

    snprintf(url, sizeof(url), MARKET_BINANCE_BASE_URL "/api/v3/ticker/24hr?symbol=%s",
             market_pair_symbol(pair));
    err = market_http_get_json(url, MARKET_BINANCE_SUMMARY_CAPACITY, &body);
    if (err != ESP_OK) {
        return err;
    }

    err = parse_summary_body(body, out);
    free(body);
    return err;
}

static esp_err_t market_feed_binance_fetch_candles(market_pair_id_t pair,
                                                   market_interval_id_t interval,
                                                   market_candle_window_t *out)
{
    char url[MARKET_BINANCE_URL_LEN];
    char *body = NULL;
    esp_err_t err;

    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, "market_feed_binance",
                        "candle out is required");

    snprintf(url, sizeof(url),
             MARKET_BINANCE_BASE_URL "/api/v3/klines?symbol=%s&interval=%s&limit=%u",
             market_pair_symbol(pair), market_interval_symbol(interval), MARKET_MAX_CANDLES);
    err = market_http_get_json(url, MARKET_BINANCE_CANDLES_CAPACITY, &body);
    if (err != ESP_OK) {
        return err;
    }

    err = parse_candles_body(body, out);
    free(body);
    return err;
}

const market_feed_iface_t *market_feed_binance_get_iface(void)
{
    static const market_feed_iface_t iface = {
        .source = MARKET_SOURCE_BINANCE,
        .source_label = "BINANCE",
        .transport_hint = market_feed_binance_transport_hint,
        .fetch_summary = market_feed_binance_fetch_summary,
        .fetch_candles = market_feed_binance_fetch_candles,
    };

    return &iface;
}
