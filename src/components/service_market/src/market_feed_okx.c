#include "market_feed.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "market_http.h"

#define MARKET_OKX_BASE_URL "https://www.okx.com"
#define MARKET_OKX_URL_LEN 256
#define MARKET_OKX_SUMMARY_CAPACITY 4096
#define MARKET_OKX_CANDLES_CAPACITY 16384

static const char *market_pair_symbol(market_pair_id_t pair)
{
    switch (pair) {
    case MARKET_PAIR_ETH_USDT:
        return "ETH-USDT";
    case MARKET_PAIR_BTC_ETH:
        return "BTC-ETH";
    case MARKET_PAIR_BTC_USDT:
    default:
        return "BTC-USDT";
    }
}

static const char *market_interval_symbol(market_interval_id_t interval)
{
    switch (interval) {
    case MARKET_INTERVAL_4H:
        return "4H";
    case MARKET_INTERVAL_1D:
        return "1Dutc";
    case MARKET_INTERVAL_1H:
    default:
        return "1H";
    }
}

static const cJSON *okx_first_data_item(cJSON *root)
{
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");

    if (!cJSON_IsArray(data) || cJSON_GetArraySize(data) <= 0) {
        return NULL;
    }

    return cJSON_GetArrayItem(data, 0);
}

static esp_err_t parse_summary_body(const char *body, market_feed_summary_t *out)
{
    cJSON *root = NULL;
    const cJSON *item = NULL;
    const char *last_str = NULL;
    const char *open24h_str = NULL;
    double last_value;
    double open24h_value;
    double change_percent;

    ESP_RETURN_ON_FALSE(body != NULL, ESP_ERR_INVALID_ARG, "market_feed_okx", "body is required");
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, "market_feed_okx",
                        "summary out is required");

    root = cJSON_Parse(body);
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_INVALID_RESPONSE, "market_feed_okx",
                        "summary JSON parse failed");

    item = okx_first_data_item(root);
    if (item == NULL || !cJSON_IsObject(item)) {
        cJSON_Delete(root);
        ESP_RETURN_ON_FALSE(false, ESP_ERR_INVALID_RESPONSE, "market_feed_okx",
                            "summary data missing");
    }

    last_str = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "last"));
    open24h_str = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "open24h"));
    if (last_str == NULL || open24h_str == NULL) {
        cJSON_Delete(root);
        ESP_RETURN_ON_FALSE(false, ESP_ERR_INVALID_RESPONSE, "market_feed_okx",
                            "summary fields missing");
    }

    last_value = strtod(last_str, NULL);
    open24h_value = strtod(open24h_str, NULL);
    change_percent = 0.0;
    if (open24h_value > 0.0) {
        change_percent = ((last_value - open24h_value) / open24h_value) * 100.0;
    }

    out->last_price_scaled = market_scale_double(last_value, 10000U);
    out->change_bp = market_scale_double(change_percent, 100U);

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t parse_candles_body(const char *body, market_candle_window_t *out)
{
    cJSON *root = NULL;
    const cJSON *data = NULL;
    int count = 0;
    int src_idx = 0;
    int dst_idx = 0;

    ESP_RETURN_ON_FALSE(body != NULL, ESP_ERR_INVALID_ARG, "market_feed_okx", "body is required");
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, "market_feed_okx",
                        "candle out is required");

    root = cJSON_Parse(body);
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_INVALID_RESPONSE, "market_feed_okx",
                        "candle JSON parse failed");

    data = cJSON_GetObjectItemCaseSensitive(root, "data");
    ESP_RETURN_ON_FALSE(cJSON_IsArray(data), ESP_ERR_INVALID_RESPONSE, "market_feed_okx",
                        "candle data missing");

    memset(out, 0, sizeof(*out));
    count = cJSON_GetArraySize(data);
    if (count > MARKET_MAX_CANDLES) {
        count = MARKET_MAX_CANDLES;
    }

    for (src_idx = count - 1, dst_idx = 0; src_idx >= 0; --src_idx, ++dst_idx) {
        const cJSON *entry = cJSON_GetArrayItem(data, src_idx);
        const cJSON *open_time = NULL;
        const char *open_str = NULL;
        const char *high_str = NULL;
        const char *low_str = NULL;
        const char *close_str = NULL;
        const char *volume_str = NULL;

        if (!cJSON_IsArray(entry) || cJSON_GetArraySize(entry) < 6) {
            cJSON_Delete(root);
            ESP_RETURN_ON_FALSE(false, ESP_ERR_INVALID_RESPONSE, "market_feed_okx",
                                "candle entry shape is invalid");
        }

        open_time = cJSON_GetArrayItem(entry, 0);
        open_str = cJSON_GetStringValue(cJSON_GetArrayItem(entry, 1));
        high_str = cJSON_GetStringValue(cJSON_GetArrayItem(entry, 2));
        low_str = cJSON_GetStringValue(cJSON_GetArrayItem(entry, 3));
        close_str = cJSON_GetStringValue(cJSON_GetArrayItem(entry, 4));
        volume_str = cJSON_GetStringValue(cJSON_GetArrayItem(entry, 5));
        if (!cJSON_IsString(open_time) || open_str == NULL || high_str == NULL || low_str == NULL ||
            close_str == NULL || volume_str == NULL) {
            cJSON_Delete(root);
            ESP_RETURN_ON_FALSE(false, ESP_ERR_INVALID_RESPONSE, "market_feed_okx",
                                "candle fields missing");
        }

        out->candles[dst_idx].open_time_epoch_s =
            (uint32_t)(strtod(cJSON_GetStringValue(open_time), NULL) / 1000.0);
        out->candles[dst_idx].open_scaled = market_scale_double(strtod(open_str, NULL), 10000U);
        out->candles[dst_idx].high_scaled = market_scale_double(strtod(high_str, NULL), 10000U);
        out->candles[dst_idx].low_scaled = market_scale_double(strtod(low_str, NULL), 10000U);
        out->candles[dst_idx].close_scaled = market_scale_double(strtod(close_str, NULL), 10000U);
        out->candles[dst_idx].volume_scaled =
            market_scale_positive_double(strtod(volume_str, NULL), 100U);
    }

    out->count = (uint16_t)count;
    cJSON_Delete(root);
    return ESP_OK;
}

static market_transport_hint_t market_feed_okx_transport_hint(void)
{
    return MARKET_TRANSPORT_POLLING;
}

static esp_err_t market_feed_okx_fetch_summary(market_pair_id_t pair, market_feed_summary_t *out)
{
    char url[MARKET_OKX_URL_LEN];
    char *body = NULL;
    esp_err_t err;

    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, "market_feed_okx",
                        "summary out is required");

    snprintf(url, sizeof(url), MARKET_OKX_BASE_URL "/api/v5/market/ticker?instId=%s",
             market_pair_symbol(pair));
    err = market_http_get_json(url, MARKET_OKX_SUMMARY_CAPACITY, &body);
    if (err != ESP_OK) {
        return err;
    }

    err = parse_summary_body(body, out);
    free(body);
    return err;
}

static esp_err_t market_feed_okx_fetch_candles(market_pair_id_t pair, market_interval_id_t interval,
                                               market_candle_window_t *out)
{
    char url[MARKET_OKX_URL_LEN];
    char *body = NULL;
    esp_err_t err;

    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, "market_feed_okx",
                        "candle out is required");

    snprintf(url, sizeof(url),
             MARKET_OKX_BASE_URL "/api/v5/market/candles?instId=%s&bar=%s&limit=%u",
             market_pair_symbol(pair), market_interval_symbol(interval), MARKET_MAX_CANDLES);
    err = market_http_get_json(url, MARKET_OKX_CANDLES_CAPACITY, &body);
    if (err != ESP_OK) {
        return err;
    }

    err = parse_candles_body(body, out);
    free(body);
    return err;
}

const market_feed_iface_t *market_feed_okx_get_iface(void)
{
    static const market_feed_iface_t iface = {
        .source = MARKET_SOURCE_OKX,
        .source_label = "OKX",
        .transport_hint = market_feed_okx_transport_hint,
        .fetch_summary = market_feed_okx_fetch_summary,
        .fetch_candles = market_feed_okx_fetch_candles,
    };

    return &iface;
}
