#include "market_feed.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "market_http.h"
#include "market_pair_math.h"
#include "market_symbols.h"

#define MARKET_GATE_BASE_URL "https://api.gateio.ws/api/v4"
#define MARKET_GATE_URL_LEN 256
#define MARKET_GATE_SUMMARY_CAPACITY 4096
#define MARKET_GATE_CANDLES_CAPACITY 16384

static esp_err_t parse_summary_body(const char *body, bool invert_price, market_feed_summary_t *out)
{
    cJSON *root = NULL;
    const cJSON *item = NULL;
    const char *last_str = NULL;
    const char *change_str = NULL;
    double last_value;
    double change_percent;
    esp_err_t err = ESP_OK;

    ESP_RETURN_ON_FALSE(body != NULL, ESP_ERR_INVALID_ARG, "market_feed_gate", "body is required");
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, "market_feed_gate",
                        "summary out is required");

    root = cJSON_Parse(body);
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_INVALID_RESPONSE, "market_feed_gate",
                        "summary JSON parse failed");
    ESP_RETURN_ON_FALSE(cJSON_IsArray(root) && cJSON_GetArraySize(root) > 0,
                        ESP_ERR_INVALID_RESPONSE, "market_feed_gate", "summary data missing");

    item = cJSON_GetArrayItem(root, 0);
    if (!cJSON_IsObject(item)) {
        cJSON_Delete(root);
        ESP_RETURN_ON_FALSE(false, ESP_ERR_INVALID_RESPONSE, "market_feed_gate",
                            "summary item is invalid");
    }

    last_str = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "last"));
    change_str = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "change_percentage"));
    if (last_str == NULL || change_str == NULL) {
        cJSON_Delete(root);
        ESP_RETURN_ON_FALSE(false, ESP_ERR_INVALID_RESPONSE, "market_feed_gate",
                            "summary fields missing");
    }

    last_value = strtod(last_str, NULL);
    change_percent = strtod(change_str, NULL);
    if (invert_price) {
        err = market_invert_price_value(last_value, &last_value);
        if (err == ESP_OK) {
            err = market_invert_change_percent(change_percent, &change_percent);
        }
        if (err != ESP_OK) {
            cJSON_Delete(root);
            return err;
        }
    }

    out->last_price_scaled = market_scale_double(last_value, 10000U);
    out->change_bp = market_scale_double(change_percent, 100U);

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t parse_candles_body(const char *body, bool invert_price,
                                    market_candle_window_t *out)
{
    cJSON *root = NULL;
    const cJSON *first_entry = NULL;
    const cJSON *last_entry = NULL;
    int count = 0;
    int src_idx = 0;
    bool descending = false;

    ESP_RETURN_ON_FALSE(body != NULL, ESP_ERR_INVALID_ARG, "market_feed_gate", "body is required");
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, "market_feed_gate",
                        "candle out is required");

    root = cJSON_Parse(body);
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_INVALID_RESPONSE, "market_feed_gate",
                        "candle JSON parse failed");
    ESP_RETURN_ON_FALSE(cJSON_IsArray(root), ESP_ERR_INVALID_RESPONSE, "market_feed_gate",
                        "candle root is not an array");

    memset(out, 0, sizeof(*out));
    count = cJSON_GetArraySize(root);
    ESP_RETURN_ON_FALSE(count > 0, ESP_ERR_INVALID_RESPONSE, "market_feed_gate",
                        "candle array is empty");
    if (count > MARKET_MAX_CANDLES) {
        count = MARKET_MAX_CANDLES;
    }

    first_entry = cJSON_GetArrayItem(root, 0);
    last_entry = cJSON_GetArrayItem(root, count - 1);
    if (cJSON_IsArray(first_entry) && cJSON_IsArray(last_entry) &&
        cJSON_GetArraySize(first_entry) > 0 && cJSON_GetArraySize(last_entry) > 0) {
        const char *first_ts = cJSON_GetStringValue(cJSON_GetArrayItem(first_entry, 0));
        const char *last_ts = cJSON_GetStringValue(cJSON_GetArrayItem(last_entry, 0));
        if (first_ts != NULL && last_ts != NULL) {
            descending = strtod(first_ts, NULL) > strtod(last_ts, NULL);
        }
    }

    for (src_idx = 0; src_idx < count; ++src_idx) {
        const cJSON *entry = cJSON_GetArrayItem(root, src_idx);
        const int dst_idx = descending ? (count - 1 - src_idx) : src_idx;
        const char *open_time_str = NULL;
        const char *quote_volume_str = NULL;
        const char *close_str = NULL;
        const char *high_str = NULL;
        const char *low_str = NULL;
        const char *open_str = NULL;
        const char *base_volume_str = NULL;
        double open_value;
        double high_value;
        double low_value;
        double close_value;
        double volume_value;
        esp_err_t err = ESP_OK;

        if (!cJSON_IsArray(entry) || cJSON_GetArraySize(entry) < 7) {
            cJSON_Delete(root);
            ESP_RETURN_ON_FALSE(false, ESP_ERR_INVALID_RESPONSE, "market_feed_gate",
                                "candle entry shape is invalid");
        }

        open_time_str = cJSON_GetStringValue(cJSON_GetArrayItem(entry, 0));
        quote_volume_str = cJSON_GetStringValue(cJSON_GetArrayItem(entry, 1));
        close_str = cJSON_GetStringValue(cJSON_GetArrayItem(entry, 2));
        high_str = cJSON_GetStringValue(cJSON_GetArrayItem(entry, 3));
        low_str = cJSON_GetStringValue(cJSON_GetArrayItem(entry, 4));
        open_str = cJSON_GetStringValue(cJSON_GetArrayItem(entry, 5));
        base_volume_str = cJSON_GetStringValue(cJSON_GetArrayItem(entry, 6));
        if (open_time_str == NULL || quote_volume_str == NULL || close_str == NULL ||
            high_str == NULL || low_str == NULL || open_str == NULL || base_volume_str == NULL) {
            cJSON_Delete(root);
            ESP_RETURN_ON_FALSE(false, ESP_ERR_INVALID_RESPONSE, "market_feed_gate",
                                "candle fields missing");
        }

        open_value = strtod(open_str, NULL);
        high_value = strtod(high_str, NULL);
        low_value = strtod(low_str, NULL);
        close_value = strtod(close_str, NULL);
        volume_value = strtod(invert_price ? quote_volume_str : base_volume_str, NULL);

        out->candles[dst_idx].open_time_epoch_s = (uint32_t)strtod(open_time_str, NULL);
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

        out->candles[dst_idx].open_scaled = market_scale_double(open_value, 10000U);
        out->candles[dst_idx].high_scaled = market_scale_double(high_value, 10000U);
        out->candles[dst_idx].low_scaled = market_scale_double(low_value, 10000U);
        out->candles[dst_idx].close_scaled = market_scale_double(close_value, 10000U);
        out->candles[dst_idx].volume_scaled = market_scale_positive_double(volume_value, 100U);
    }

    out->count = (uint16_t)count;
    cJSON_Delete(root);
    return ESP_OK;
}

static market_transport_hint_t market_feed_gate_transport_hint(void)
{
    return MARKET_TRANSPORT_POLLING;
}

static esp_err_t market_feed_gate_fetch_summary(market_pair_id_t pair, market_feed_summary_t *out)
{
    market_symbol_config_t config;
    char url[MARKET_GATE_URL_LEN];
    char *body = NULL;
    esp_err_t err;

    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, "market_feed_gate",
                        "summary out is required");
    ESP_RETURN_ON_FALSE(market_source_pair_config(MARKET_SOURCE_GATE, pair, &config),
                        ESP_ERR_INVALID_ARG, "market_feed_gate", "pair is invalid");

    snprintf(url, sizeof(url), MARKET_GATE_BASE_URL "/spot/tickers?currency_pair=%s",
             config.symbol);
    err = market_http_get_json(url, MARKET_GATE_SUMMARY_CAPACITY, &body);
    if (err != ESP_OK) {
        return err;
    }

    err = parse_summary_body(body, config.invert_price, out);
    free(body);
    return err;
}

static esp_err_t market_feed_gate_fetch_candles(market_pair_id_t pair,
                                                market_interval_id_t interval,
                                                market_candle_window_t *out)
{
    market_symbol_config_t config;
    char url[MARKET_GATE_URL_LEN];
    char *body = NULL;
    esp_err_t err;

    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, "market_feed_gate",
                        "candle out is required");
    ESP_RETURN_ON_FALSE(market_source_pair_config(MARKET_SOURCE_GATE, pair, &config),
                        ESP_ERR_INVALID_ARG, "market_feed_gate", "pair is invalid");

    snprintf(url, sizeof(url),
             MARKET_GATE_BASE_URL "/spot/candlesticks?currency_pair=%s&interval=%s&limit=%u",
             config.symbol, market_interval_symbol(interval), MARKET_MAX_CANDLES);
    err = market_http_get_json(url, MARKET_GATE_CANDLES_CAPACITY, &body);
    if (err != ESP_OK) {
        return err;
    }

    err = parse_candles_body(body, config.invert_price, out);
    free(body);
    return err;
}

const market_feed_iface_t *market_feed_gate_get_iface(void)
{
    static const market_feed_iface_t iface = {
        .source = MARKET_SOURCE_GATE,
        .source_label = "GATE",
        .transport_hint = market_feed_gate_transport_hint,
        .fetch_summary = market_feed_gate_fetch_summary,
        .fetch_candles = market_feed_gate_fetch_candles,
    };

    return &iface;
}
