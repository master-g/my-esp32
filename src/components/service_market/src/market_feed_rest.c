#include "market_feed.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "market_http.h"
#include "market_pair_math.h"
#include "market_symbols.h"

#define MARKET_BINANCE_BASE_URL "https://api.binance.com"
#define MARKET_BINANCE_URL_LEN 256
#define MARKET_BINANCE_SUMMARY_CAPACITY 4096
#define MARKET_BINANCE_CANDLES_CAPACITY 16384

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

    out->last_price_scaled = market_scale_double(strtod(last_price_str, NULL), 10000U);
    out->change_bp = market_scale_double(strtod(change_percent_str, NULL), 100U);

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t parse_summary_body_with_pair(const char *body, bool invert_price,
                                              market_feed_summary_t *out)
{
    esp_err_t err;
    double last_value;
    double change_percent;

    err = parse_summary_body(body, out);
    if (err != ESP_OK || !invert_price) {
        return err;
    }

    last_value = (double)out->last_price_scaled / 10000.0;
    change_percent = (double)out->change_bp / 100.0;
    err = market_invert_price_value(last_value, &last_value);
    if (err == ESP_OK) {
        err = market_invert_change_percent(change_percent, &change_percent);
    }
    if (err != ESP_OK) {
        return err;
    }

    out->last_price_scaled = market_scale_double(last_value, 10000U);
    out->change_bp = market_scale_double(change_percent, 100U);
    return ESP_OK;
}

static esp_err_t parse_candles_body(const char *body, bool invert_price,
                                    market_candle_window_t *out)
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
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        ESP_RETURN_ON_FALSE(false, ESP_ERR_INVALID_RESPONSE, "market_feed_binance",
                            "candle root is not an array");
    }

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
        const char *base_volume_str = NULL;
        const char *quote_volume_str = NULL;
        double open_value;
        double high_value;
        double low_value;
        double close_value;
        double volume_value;
        esp_err_t err = ESP_OK;

        if (!cJSON_IsArray(entry) || cJSON_GetArraySize(entry) < 8) {
            cJSON_Delete(root);
            ESP_RETURN_ON_FALSE(false, ESP_ERR_INVALID_RESPONSE, "market_feed_binance",
                                "candle entry shape is invalid");
        }

        open_time = cJSON_GetArrayItem(entry, 0);
        open_str = cJSON_GetStringValue(cJSON_GetArrayItem(entry, 1));
        high_str = cJSON_GetStringValue(cJSON_GetArrayItem(entry, 2));
        low_str = cJSON_GetStringValue(cJSON_GetArrayItem(entry, 3));
        close_str = cJSON_GetStringValue(cJSON_GetArrayItem(entry, 4));
        base_volume_str = cJSON_GetStringValue(cJSON_GetArrayItem(entry, 5));
        quote_volume_str = cJSON_GetStringValue(cJSON_GetArrayItem(entry, 7));
        if (!cJSON_IsNumber(open_time) || open_str == NULL || high_str == NULL || low_str == NULL ||
            close_str == NULL || base_volume_str == NULL || quote_volume_str == NULL) {
            cJSON_Delete(root);
            ESP_RETURN_ON_FALSE(false, ESP_ERR_INVALID_RESPONSE, "market_feed_binance",
                                "candle fields missing");
        }

        open_value = strtod(open_str, NULL);
        high_value = strtod(high_str, NULL);
        low_value = strtod(low_str, NULL);
        close_value = strtod(close_str, NULL);
        volume_value = strtod(invert_price ? quote_volume_str : base_volume_str, NULL);

        out->candles[i].open_time_epoch_s = (uint32_t)(cJSON_GetNumberValue(open_time) / 1000.0);
        if (invert_price) {
            const double direct_high = high_value;
            const double direct_low = low_value;

            err = market_invert_price_value(open_value, &open_value);
            if (err == ESP_OK) {
                err = market_invert_price_value(direct_low, &high_value);
            }
            if (err == ESP_OK) {
                err = market_invert_price_value(direct_high, &low_value);
            }
            if (err == ESP_OK) {
                err = market_invert_price_value(close_value, &close_value);
            }
            if (err != ESP_OK) {
                cJSON_Delete(root);
                return err;
            }
        }

        out->candles[i].open_scaled = market_scale_double(open_value, 10000U);
        out->candles[i].high_scaled = market_scale_double(high_value, 10000U);
        out->candles[i].low_scaled = market_scale_double(low_value, 10000U);
        out->candles[i].close_scaled = market_scale_double(close_value, 10000U);
        out->candles[i].volume_scaled = market_scale_positive_double(volume_value, 100U);
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
    market_symbol_config_t config;
    char url[MARKET_BINANCE_URL_LEN];
    char *body = NULL;
    esp_err_t err;

    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, "market_feed_binance",
                        "summary out is required");
    ESP_RETURN_ON_FALSE(market_source_pair_config(MARKET_SOURCE_BINANCE, pair, &config),
                        ESP_ERR_INVALID_ARG, "market_feed_binance", "pair is invalid");

    snprintf(url, sizeof(url), MARKET_BINANCE_BASE_URL "/api/v3/ticker/24hr?symbol=%s",
             config.symbol);
    err = market_http_get_json(url, MARKET_BINANCE_SUMMARY_CAPACITY, &body);
    if (err != ESP_OK) {
        return err;
    }

    err = parse_summary_body_with_pair(body, config.invert_price, out);
    free(body);
    return err;
}

static esp_err_t market_feed_binance_fetch_candles(market_pair_id_t pair,
                                                   market_interval_id_t interval,
                                                   market_candle_window_t *out)
{
    market_symbol_config_t config;
    char url[MARKET_BINANCE_URL_LEN];
    char *body = NULL;
    esp_err_t err;

    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, "market_feed_binance",
                        "candle out is required");
    ESP_RETURN_ON_FALSE(market_source_pair_config(MARKET_SOURCE_BINANCE, pair, &config),
                        ESP_ERR_INVALID_ARG, "market_feed_binance", "pair is invalid");

    snprintf(url, sizeof(url),
             MARKET_BINANCE_BASE_URL "/api/v3/klines?symbol=%s&interval=%s&limit=%u", config.symbol,
             market_interval_symbol(interval), MARKET_MAX_CANDLES);
    err = market_http_get_json(url, MARKET_BINANCE_CANDLES_CAPACITY, &body);
    if (err != ESP_OK) {
        return err;
    }

    err = parse_candles_body(body, config.invert_price, out);
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
