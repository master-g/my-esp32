#include "market_stream_binance.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "market_http.h"
#include "market_pair_math.h"
#include "market_symbols.h"

#define MARKET_BINANCE_WS_URI_LEN 192
#define MARKET_BINANCE_WS_FRAME_CAPACITY 1536
#define MARKET_BINANCE_WS_ROTATE_US (23LL * 60 * 60 * 1000 * 1000)
#define MARKET_BINANCE_WS_TASK_STACK (8 * 1024)

typedef struct {
    SemaphoreHandle_t mutex;
    market_binance_stream_callbacks_t callbacks;
    void *callback_context;
    esp_websocket_client_handle_t client;
    market_pair_id_t pair;
    market_interval_id_t interval;
    bool initialized;
    bool connected;
    bool suppress_disconnect;
    int64_t connected_at_us;
    char uri[MARKET_BINANCE_WS_URI_LEN];
    char frame_buffer[MARKET_BINANCE_WS_FRAME_CAPACITY];
} market_binance_stream_state_t;

static const char *TAG = "market_stream_binance";
static market_binance_stream_state_t s_stream;

static void build_lowercase_symbol(const char *symbol, char *out, size_t out_size)
{
    size_t i = 0;

    if (out == NULL || out_size == 0) {
        return;
    }
    if (symbol == NULL) {
        out[0] = '\0';
        return;
    }

    for (i = 0; symbol[i] != '\0' && i + 1U < out_size; ++i) {
        out[i] = (char)tolower((unsigned char)symbol[i]);
    }
    out[i] = '\0';
}

static esp_err_t build_stream_uri(market_pair_id_t pair, market_interval_id_t interval, char *out,
                                  size_t out_size)
{
    market_symbol_config_t config;
    char symbol_lower[16];

    ESP_RETURN_ON_FALSE(out != NULL && out_size > 0, ESP_ERR_INVALID_ARG, TAG,
                        "stream uri out is required");
    ESP_RETURN_ON_FALSE(market_source_pair_config(MARKET_SOURCE_BINANCE, pair, &config),
                        ESP_ERR_INVALID_ARG, TAG, "pair is invalid");

    build_lowercase_symbol(config.symbol, symbol_lower, sizeof(symbol_lower));
    snprintf(out, out_size, "wss://data-stream.binance.vision/stream?streams=%s@ticker/%s@kline_%s",
             symbol_lower, symbol_lower, market_interval_symbol(interval));
    return ESP_OK;
}

static esp_err_t parse_summary_event(const cJSON *data, bool invert_price,
                                     market_feed_summary_t *out)
{
    const char *last_str = NULL;
    const char *change_str = NULL;
    double last_value;
    double change_percent;
    esp_err_t err = ESP_OK;

    ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_INVALID_ARG, TAG, "summary data is required");
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "summary out is required");

    last_str = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(data, "c"));
    change_str = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(data, "P"));
    ESP_RETURN_ON_FALSE(last_str != NULL && change_str != NULL, ESP_ERR_INVALID_RESPONSE, TAG,
                        "summary fields missing");

    last_value = strtod(last_str, NULL);
    change_percent = strtod(change_str, NULL);
    if (invert_price) {
        err = market_invert_price_value(last_value, &last_value);
        if (err == ESP_OK) {
            err = market_invert_change_percent(change_percent, &change_percent);
        }
        if (err != ESP_OK) {
            return err;
        }
    }

    out->last_price_scaled = market_scale_double(last_value, 10000U);
    out->change_bp = market_scale_double(change_percent, 100U);
    return ESP_OK;
}

static esp_err_t parse_kline_event(const cJSON *data, bool invert_price, market_candle_t *out)
{
    const cJSON *kline = NULL;
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

    ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_INVALID_ARG, TAG, "kline data is required");
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "kline out is required");

    kline = cJSON_GetObjectItemCaseSensitive(data, "k");
    ESP_RETURN_ON_FALSE(cJSON_IsObject(kline), ESP_ERR_INVALID_RESPONSE, TAG,
                        "kline object missing");

    open_time = cJSON_GetObjectItemCaseSensitive(kline, "t");
    open_str = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(kline, "o"));
    high_str = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(kline, "h"));
    low_str = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(kline, "l"));
    close_str = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(kline, "c"));
    base_volume_str = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(kline, "v"));
    quote_volume_str = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(kline, "q"));
    ESP_RETURN_ON_FALSE(cJSON_IsNumber(open_time) && open_str != NULL && high_str != NULL &&
                            low_str != NULL && close_str != NULL && base_volume_str != NULL &&
                            quote_volume_str != NULL,
                        ESP_ERR_INVALID_RESPONSE, TAG, "kline fields missing");

    open_value = strtod(open_str, NULL);
    high_value = strtod(high_str, NULL);
    low_value = strtod(low_str, NULL);
    close_value = strtod(close_str, NULL);
    volume_value = strtod(invert_price ? quote_volume_str : base_volume_str, NULL);
    out->open_time_epoch_s = (uint32_t)(cJSON_GetNumberValue(open_time) / 1000.0);

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
            return err;
        }
    }

    out->open_scaled = market_scale_double(open_value, 10000U);
    out->high_scaled = market_scale_double(high_value, 10000U);
    out->low_scaled = market_scale_double(low_value, 10000U);
    out->close_scaled = market_scale_double(close_value, 10000U);
    out->volume_scaled = market_scale_positive_double(volume_value, 100U);
    return ESP_OK;
}

static void dispatch_payload(const char *payload, market_pair_id_t pair,
                             market_interval_id_t interval,
                             const market_binance_stream_callbacks_t *callbacks, void *context)
{
    market_symbol_config_t config;
    cJSON *root = NULL;
    const cJSON *data = NULL;
    const char *event_type = NULL;

    if (payload == NULL || callbacks == NULL ||
        !market_source_pair_config(MARKET_SOURCE_BINANCE, pair, &config)) {
        return;
    }

    root = cJSON_Parse(payload);
    if (root == NULL) {
        ESP_LOGW(TAG, "stream JSON parse failed");
        return;
    }

    data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsObject(data)) {
        data = root;
    }
    if (!cJSON_IsObject(data)) {
        cJSON_Delete(root);
        return;
    }

    event_type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(data, "e"));
    if (event_type != NULL && strcmp(event_type, "24hrTicker") == 0) {
        market_feed_summary_t summary = {0};

        if (parse_summary_event(data, config.invert_price, &summary) == ESP_OK &&
            callbacks->on_summary != NULL) {
            callbacks->on_summary(pair, &summary, context);
        }
    } else if (event_type != NULL && strcmp(event_type, "kline") == 0) {
        market_candle_t candle = {0};

        if (parse_kline_event(data, config.invert_price, &candle) == ESP_OK &&
            callbacks->on_kline != NULL) {
            callbacks->on_kline(pair, interval, &candle, context);
        }
    }

    cJSON_Delete(root);
}

static void market_binance_stream_event_handler(void *handler_args, esp_event_base_t base,
                                                int32_t event_id, void *event_data)
{
    esp_websocket_client_handle_t event_client = (esp_websocket_client_handle_t)handler_args;
    market_binance_stream_callbacks_t callbacks = {0};
    void *callback_context = NULL;
    market_pair_id_t pair = MARKET_PAIR_BTC_USDT;
    market_interval_id_t interval = MARKET_INTERVAL_5M;
    esp_err_t last_esp_err = ESP_OK;
    int status_code = 0;
    bool suppress_disconnect = false;
    bool notify_connected = false;
    bool notify_disconnected = false;
    char payload[MARKET_BINANCE_WS_FRAME_CAPACITY];
    bool dispatch = false;

    (void)base;
    if (s_stream.mutex == NULL) {
        return;
    }

    xSemaphoreTake(s_stream.mutex, portMAX_DELAY);
    if (event_client == NULL || event_client != s_stream.client) {
        xSemaphoreGive(s_stream.mutex);
        return;
    }

    callbacks = s_stream.callbacks;
    callback_context = s_stream.callback_context;
    pair = s_stream.pair;
    interval = s_stream.interval;
    suppress_disconnect = s_stream.suppress_disconnect;

    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        s_stream.connected = true;
        s_stream.connected_at_us = esp_timer_get_time();
        notify_connected = true;
    } else if (event_id == WEBSOCKET_EVENT_DATA) {
        esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
        int copied_len = 0;

        if (data == NULL || data->op_code != WS_TRANSPORT_OPCODES_TEXT || data->data_len <= 0 ||
            data->payload_len <= 0 || data->payload_len >= (int)sizeof(s_stream.frame_buffer)) {
            xSemaphoreGive(s_stream.mutex);
            return;
        }
        if (data->payload_offset < 0 ||
            (data->payload_offset + data->data_len) > (int)sizeof(s_stream.frame_buffer) - 1) {
            xSemaphoreGive(s_stream.mutex);
            return;
        }

        if (data->payload_offset == 0) {
            memset(s_stream.frame_buffer, 0, sizeof(s_stream.frame_buffer));
        }
        memcpy(s_stream.frame_buffer + data->payload_offset, data->data_ptr,
               (size_t)data->data_len);
        copied_len = data->payload_offset + data->data_len;
        if (data->fin && copied_len >= data->payload_len) {
            s_stream.frame_buffer[data->payload_len] = '\0';
            memcpy(payload, s_stream.frame_buffer, (size_t)data->payload_len + 1U);
            dispatch = true;
        }
    } else if (event_id == WEBSOCKET_EVENT_ERROR) {
        esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

        s_stream.connected = false;
        if (data != NULL) {
            last_esp_err = data->error_handle.esp_tls_last_esp_err;
            status_code = data->error_handle.esp_ws_handshake_status_code;
        }
        notify_disconnected = !suppress_disconnect;
    } else if (event_id == WEBSOCKET_EVENT_DISCONNECTED || event_id == WEBSOCKET_EVENT_CLOSED) {
        s_stream.connected = false;
        notify_disconnected = !suppress_disconnect;
    }
    xSemaphoreGive(s_stream.mutex);

    if (notify_connected && callbacks.on_connected != NULL) {
        callbacks.on_connected(pair, interval, callback_context);
    }
    if (notify_disconnected && callbacks.on_disconnected != NULL) {
        callbacks.on_disconnected(pair, interval, last_esp_err, status_code, callback_context);
    }
    if (dispatch) {
        dispatch_payload(payload, pair, interval, &callbacks, callback_context);
    }
}

esp_err_t market_binance_stream_init(const market_binance_stream_callbacks_t *callbacks,
                                     void *context)
{
    if (callbacks == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_stream.mutex == NULL) {
        s_stream.mutex = xSemaphoreCreateMutex();
        if (s_stream.mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_stream.mutex, portMAX_DELAY);
    s_stream.callbacks = *callbacks;
    s_stream.callback_context = context;
    s_stream.initialized = true;
    xSemaphoreGive(s_stream.mutex);
    return ESP_OK;
}

void market_binance_stream_deinit(void)
{
    if (s_stream.mutex == NULL) {
        return;
    }

    market_binance_stream_stop();
    xSemaphoreTake(s_stream.mutex, portMAX_DELAY);
    memset(&s_stream.callbacks, 0, sizeof(s_stream.callbacks));
    s_stream.callback_context = NULL;
    s_stream.initialized = false;
    xSemaphoreGive(s_stream.mutex);
}

esp_err_t market_binance_stream_start(market_pair_id_t pair, market_interval_id_t interval)
{
    esp_websocket_client_handle_t client;
    esp_websocket_client_config_t config = {0};
    char uri[MARKET_BINANCE_WS_URI_LEN];
    int64_t now_us = esp_timer_get_time();

    if (s_stream.mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_FALSE(build_stream_uri(pair, interval, uri, sizeof(uri)) == ESP_OK,
                        ESP_ERR_INVALID_ARG, TAG, "stream uri build failed");

    xSemaphoreTake(s_stream.mutex, portMAX_DELAY);
    if (!s_stream.initialized) {
        xSemaphoreGive(s_stream.mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_stream.client != NULL && s_stream.pair == pair && s_stream.interval == interval &&
        strcmp(s_stream.uri, uri) == 0 &&
        (!s_stream.connected ||
         (now_us - s_stream.connected_at_us) < MARKET_BINANCE_WS_ROTATE_US)) {
        xSemaphoreGive(s_stream.mutex);
        return ESP_OK;
    }
    xSemaphoreGive(s_stream.mutex);

    market_binance_stream_stop();

    memset(&config, 0, sizeof(config));
    config.uri = uri;
    config.buffer_size = MARKET_BINANCE_WS_FRAME_CAPACITY - 1;
    config.task_stack = MARKET_BINANCE_WS_TASK_STACK;
    config.disable_auto_reconnect = true;
    config.network_timeout_ms = 10000;
    config.pingpong_timeout_sec = 60;
    config.keep_alive_enable = true;
    config.keep_alive_idle = 15;
    config.keep_alive_interval = 15;
    config.keep_alive_count = 3;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    client = esp_websocket_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY,
                                      market_binance_stream_event_handler, client) != ESP_OK) {
        esp_websocket_client_destroy(client);
        return ESP_FAIL;
    }

    xSemaphoreTake(s_stream.mutex, portMAX_DELAY);
    s_stream.client = client;
    s_stream.pair = pair;
    s_stream.interval = interval;
    s_stream.connected = false;
    s_stream.suppress_disconnect = false;
    s_stream.connected_at_us = 0;
    snprintf(s_stream.uri, sizeof(s_stream.uri), "%s", uri);
    memset(s_stream.frame_buffer, 0, sizeof(s_stream.frame_buffer));
    xSemaphoreGive(s_stream.mutex);

    if (esp_websocket_client_start(client) != ESP_OK) {
        market_binance_stream_stop();
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "starting stream for %s %s", market_pair_label(pair),
             market_interval_label(interval));
    return ESP_OK;
}

void market_binance_stream_stop(void)
{
    esp_websocket_client_handle_t client = NULL;

    if (s_stream.mutex == NULL) {
        return;
    }

    xSemaphoreTake(s_stream.mutex, portMAX_DELAY);
    client = s_stream.client;
    s_stream.client = NULL;
    s_stream.connected = false;
    s_stream.suppress_disconnect = true;
    s_stream.connected_at_us = 0;
    s_stream.uri[0] = '\0';
    memset(s_stream.frame_buffer, 0, sizeof(s_stream.frame_buffer));
    xSemaphoreGive(s_stream.mutex);

    if (client == NULL) {
        return;
    }

    (void)esp_websocket_client_close(client, pdMS_TO_TICKS(1000));
    (void)esp_websocket_client_destroy(client);
}

bool market_binance_stream_is_connected(void)
{
    bool connected = false;

    if (s_stream.mutex == NULL) {
        return false;
    }

    xSemaphoreTake(s_stream.mutex, portMAX_DELAY);
    connected = s_stream.connected;
    xSemaphoreGive(s_stream.mutex);
    return connected;
}
