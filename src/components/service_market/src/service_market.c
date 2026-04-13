#include "service_market.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "core_types/app_event.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "market_feed.h"
#include "market_stream_binance.h"
#include "net_manager.h"
#include "power_policy.h"

#define MARKET_SERVICE_QUEUE_LEN 16
#define MARKET_SERVICE_TASK_STACK (20 * 1024)
#define MARKET_SUMMARY_STALE_US (30LL * 1000 * 1000)
#define MARKET_SUMMARY_REALTIME_US (5LL * 1000 * 1000)
#define MARKET_SUMMARY_INTERACTIVE_US (20LL * 1000 * 1000)
#define MARKET_CHART_REALTIME_US (60LL * 1000 * 1000)
#define MARKET_CHART_INTERACTIVE_US (120LL * 1000 * 1000)
#define MARKET_PRIMARY_RETRY_US (60LL * 1000 * 1000)
#define MARKET_STREAM_RETRY_US (15LL * 1000 * 1000)

typedef enum {
    MARKET_CMD_REFRESH_SUMMARY = 1,
    MARKET_CMD_REFRESH_CHART,
    MARKET_CMD_STREAM_CONNECTED,
    MARKET_CMD_STREAM_DISCONNECTED,
    MARKET_CMD_STREAM_SUMMARY,
    MARKET_CMD_STREAM_KLINE,
} market_command_type_t;

typedef struct {
    market_command_type_t type;
    market_pair_id_t pair;
    market_interval_id_t interval;
    market_feed_summary_t summary;
    market_candle_t candle;
    esp_err_t error;
    int status_code;
} market_command_t;

typedef struct {
    bool valid;
    bool loading;
    bool had_error;
    uint32_t updated_at_epoch_s;
    int64_t updated_at_monotonic_us;
    uint32_t last_error_epoch_s;
    int32_t last_price_scaled;
    int32_t change_bp;
    char price_text[24];
    char change_text[16];
} market_summary_cache_t;

typedef struct {
    bool valid;
    bool loading;
    bool had_error;
    uint32_t updated_at_epoch_s;
    uint32_t last_error_epoch_s;
    market_candle_window_t window;
} market_chart_cache_t;

static const char *TAG = "market_service";
static SemaphoreHandle_t s_mutex;
static QueueHandle_t s_command_queue;
static const market_feed_iface_t *s_feeds[MARKET_SOURCE_COUNT];
static market_selection_t s_selection;
static refresh_mode_t s_refresh_mode;
static market_transport_hint_t s_transport_hint;
static market_source_t s_active_source;
static bool s_fallback_active;
static bool s_stream_connected;
static uint8_t s_source_error_count;
static int64_t s_primary_retry_after_us;
static int64_t s_stream_retry_after_us;
static market_summary_cache_t s_summaries[MARKET_PAIR_COUNT];
static market_chart_cache_t s_charts[MARKET_PAIR_COUNT][MARKET_INTERVAL_COUNT];
static int64_t s_last_summary_request_us[MARKET_PAIR_COUNT];
static int64_t s_last_chart_request_us[MARKET_PAIR_COUNT][MARKET_INTERVAL_COUNT];
static bool s_summary_pending[MARKET_PAIR_COUNT];
static bool s_chart_pending[MARKET_PAIR_COUNT][MARKET_INTERVAL_COUNT];

static bool market_pair_is_valid(market_pair_id_t pair)
{
    return pair >= MARKET_PAIR_BTC_USDT && pair <= MARKET_PAIR_BTC_ETH;
}

static bool market_interval_is_valid(market_interval_id_t interval)
{
    return interval >= MARKET_INTERVAL_1H && interval <= MARKET_INTERVAL_1D;
}

const char *market_pair_label(market_pair_id_t pair)
{
    switch (pair) {
    case MARKET_PAIR_ETH_USDT:
        return "ETH/USDT";
    case MARKET_PAIR_BTC_ETH:
        return "BTC/ETH";
    case MARKET_PAIR_BTC_USDT:
    default:
        return "BTC/USDT";
    }
}

const char *market_interval_label(market_interval_id_t interval)
{
    switch (interval) {
    case MARKET_INTERVAL_4H:
        return "4H";
    case MARKET_INTERVAL_1D:
        return "1D";
    case MARKET_INTERVAL_1H:
    default:
        return "1H";
    }
}

const char *market_source_label(market_source_t source)
{
    switch (source) {
    case MARKET_SOURCE_BINANCE:
        return "BINANCE";
    case MARKET_SOURCE_GATE:
        return "GATE";
    default:
        return "UNKNOWN";
    }
}

static uint8_t market_pair_price_decimals(market_pair_id_t pair)
{
    return (pair == MARKET_PAIR_BTC_ETH) ? 4U : 2U;
}

static int32_t pow10_i32(uint8_t exp)
{
    int32_t value = 1;
    uint8_t i = 0;

    for (i = 0; i < exp; ++i) {
        value *= 10;
    }

    return value;
}

static uint32_t current_epoch_s(void)
{
    time_t now = time(NULL);

    if (now <= 1700000000) {
        return 0;
    }

    return (uint32_t)now;
}

static void format_price_text(char *out, size_t out_size, market_pair_id_t pair, int32_t scaled)
{
    const uint8_t decimals = market_pair_price_decimals(pair);
    int32_t abs_scaled = scaled;
    int32_t visible_divisor = pow10_i32(decimals);
    int32_t rounding_unit = pow10_i32((uint8_t)(4U - decimals));
    int32_t rounded_units;
    int32_t integer;
    int32_t fraction;
    bool negative = false;

    if (out == NULL || out_size == 0) {
        return;
    }

    if (abs_scaled < 0) {
        negative = true;
        abs_scaled = -abs_scaled;
    }

    rounded_units = (abs_scaled + (rounding_unit / 2)) / rounding_unit;
    integer = rounded_units / visible_divisor;
    fraction = rounded_units % visible_divisor;

    if (decimals == 0U) {
        snprintf(out, out_size, "%s%" PRId32, negative ? "-" : "", integer);
        return;
    }

    snprintf(out, out_size, "%s%" PRId32 ".%0*" PRId32, negative ? "-" : "", integer, (int)decimals,
             fraction);
}

static void format_change_text(char *out, size_t out_size, int32_t change_bp)
{
    int32_t abs_bp = change_bp;
    char sign = '+';

    if (out == NULL || out_size == 0) {
        return;
    }

    if (change_bp < 0) {
        sign = '-';
        abs_bp = -abs_bp;
    }

    snprintf(out, out_size, "%c%" PRId32 ".%02" PRId32 "%%", sign, abs_bp / 100, abs_bp % 100);
}

static void publish_market_event(void)
{
    app_event_t event = {
        .type = APP_EVENT_DATA_MARKET,
        .payload = NULL,
    };

    event_bus_publish(&event);
}

static bool refresh_mode_allows_network(refresh_mode_t mode)
{
    return mode == REFRESH_MODE_REALTIME || mode == REFRESH_MODE_INTERACTIVE_POLL;
}

static int64_t summary_refresh_interval_us(refresh_mode_t mode)
{
    switch (mode) {
    case REFRESH_MODE_REALTIME:
        return MARKET_SUMMARY_REALTIME_US;
    case REFRESH_MODE_INTERACTIVE_POLL:
        return MARKET_SUMMARY_INTERACTIVE_US;
    default:
        return 0;
    }
}

static int64_t chart_refresh_interval_us(refresh_mode_t mode)
{
    switch (mode) {
    case REFRESH_MODE_REALTIME:
        return MARKET_CHART_REALTIME_US;
    case REFRESH_MODE_INTERACTIVE_POLL:
        return MARKET_CHART_INTERACTIVE_US;
    default:
        return 0;
    }
}

static const market_feed_iface_t *feed_for_source(market_source_t source)
{
    if (source >= MARKET_SOURCE_COUNT) {
        return NULL;
    }

    return s_feeds[source];
}

static market_source_t alternate_source(market_source_t source)
{
    if (source == MARKET_SOURCE_GATE) {
        return MARKET_SOURCE_BINANCE;
    }

    return MARKET_SOURCE_GATE;
}

static market_source_t current_preferred_source(int64_t now_us)
{
    market_source_t preferred = MARKET_SOURCE_GATE;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_fallback_active && s_active_source < MARKET_SOURCE_COUNT &&
        now_us < s_primary_retry_after_us) {
        preferred = s_active_source;
    }
    xSemaphoreGive(s_mutex);

    return preferred;
}

static void note_primary_success(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_active_source = MARKET_SOURCE_GATE;
    s_fallback_active = false;
    s_source_error_count = 0;
    s_primary_retry_after_us = 0;
    xSemaphoreGive(s_mutex);
}

static void note_fallback_success(market_source_t source, market_transport_hint_t transport_hint)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_active_source = source;
    s_fallback_active = true;
    if (s_source_error_count < UINT8_MAX) {
        s_source_error_count++;
    }
    s_transport_hint = transport_hint;
    xSemaphoreGive(s_mutex);
}

static void note_refresh_failure(market_source_t failed_source)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (failed_source == MARKET_SOURCE_GATE) {
        s_primary_retry_after_us = esp_timer_get_time() + MARKET_PRIMARY_RETRY_US;
    }
    if (s_source_error_count < UINT8_MAX) {
        s_source_error_count++;
    }
    xSemaphoreGive(s_mutex);
}

static void note_active_transport(market_source_t source, market_transport_hint_t transport_hint)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_active_source = source;
    s_transport_hint = transport_hint;
    xSemaphoreGive(s_mutex);
}

static bool stream_should_run(refresh_mode_t mode, bool wifi_connected)
{
    return wifi_connected && mode == REFRESH_MODE_REALTIME;
}

static bool stream_selection_matches_locked(market_pair_id_t pair, market_interval_id_t interval)
{
    return s_selection.pair == pair && s_selection.interval == interval;
}

static void note_streaming_active_locked(void)
{
    s_active_source = MARKET_SOURCE_BINANCE;
    s_transport_hint = MARKET_TRANSPORT_STREAMING;
    s_fallback_active = false;
}

static esp_err_t fetch_summary_with_fallback(market_pair_id_t pair, market_feed_summary_t *out,
                                             market_source_t *source_out)
{
    const int64_t now_us = esp_timer_get_time();
    const market_source_t primary_source = current_preferred_source(now_us);
    const market_source_t secondary_source = alternate_source(primary_source);
    const market_feed_iface_t *primary_feed = feed_for_source(primary_source);
    const market_feed_iface_t *secondary_feed = feed_for_source(secondary_source);
    esp_err_t err;

    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "summary out is required");

    err = primary_feed->fetch_summary(pair, out);
    if (err == ESP_OK) {
        note_active_transport(primary_source, primary_feed->transport_hint());
        if (primary_source == MARKET_SOURCE_GATE) {
            note_primary_success();
        }
        if (source_out != NULL) {
            *source_out = primary_source;
        }
        return ESP_OK;
    }

    note_refresh_failure(primary_source);
    err = secondary_feed->fetch_summary(pair, out);
    if (err == ESP_OK) {
        note_fallback_success(secondary_source, secondary_feed->transport_hint());
        if (source_out != NULL) {
            *source_out = secondary_source;
        }
        return ESP_OK;
    }

    note_refresh_failure(secondary_source);
    return err;
}

static esp_err_t fetch_candles_with_fallback(market_pair_id_t pair, market_interval_id_t interval,
                                             market_candle_window_t *out,
                                             market_source_t *source_out)
{
    const int64_t now_us = esp_timer_get_time();
    const market_source_t primary_source = current_preferred_source(now_us);
    const market_source_t secondary_source = alternate_source(primary_source);
    const market_feed_iface_t *primary_feed = feed_for_source(primary_source);
    const market_feed_iface_t *secondary_feed = feed_for_source(secondary_source);
    esp_err_t err;

    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "candle out is required");

    err = primary_feed->fetch_candles(pair, interval, out);
    if (err == ESP_OK) {
        note_active_transport(primary_source, primary_feed->transport_hint());
        if (primary_source == MARKET_SOURCE_GATE) {
            note_primary_success();
        }
        if (source_out != NULL) {
            *source_out = primary_source;
        }
        return ESP_OK;
    }

    note_refresh_failure(primary_source);
    err = secondary_feed->fetch_candles(pair, interval, out);
    if (err == ESP_OK) {
        note_fallback_success(secondary_source, secondary_feed->transport_hint());
        if (source_out != NULL) {
            *source_out = secondary_source;
        }
        return ESP_OK;
    }

    note_refresh_failure(secondary_source);
    return err;
}

static bool queue_command(const market_command_t *command)
{
    if (command == NULL || s_command_queue == NULL) {
        return false;
    }

    return xQueueSend(s_command_queue, command, 0) == pdTRUE;
}

static void market_stream_connected_cb(market_pair_id_t pair, market_interval_id_t interval,
                                       void *context)
{
    const market_command_t command = {
        .type = MARKET_CMD_STREAM_CONNECTED,
        .pair = pair,
        .interval = interval,
    };

    (void)context;
    (void)queue_command(&command);
}

static void market_stream_disconnected_cb(market_pair_id_t pair, market_interval_id_t interval,
                                          esp_err_t last_esp_err, int status_code, void *context)
{
    const market_command_t command = {
        .type = MARKET_CMD_STREAM_DISCONNECTED,
        .pair = pair,
        .interval = interval,
        .error = last_esp_err,
        .status_code = status_code,
    };

    (void)context;
    (void)queue_command(&command);
}

static void market_stream_summary_cb(market_pair_id_t pair, const market_feed_summary_t *summary,
                                     void *context)
{
    market_command_t command = {
        .type = MARKET_CMD_STREAM_SUMMARY,
        .pair = pair,
        .interval = MARKET_INTERVAL_1H,
    };

    (void)context;
    if (summary == NULL) {
        return;
    }

    command.summary = *summary;
    (void)queue_command(&command);
}

static void market_stream_kline_cb(market_pair_id_t pair, market_interval_id_t interval,
                                   const market_candle_t *candle, void *context)
{
    market_command_t command = {
        .type = MARKET_CMD_STREAM_KLINE,
        .pair = pair,
        .interval = interval,
    };

    (void)context;
    if (candle == NULL) {
        return;
    }

    command.candle = *candle;
    (void)queue_command(&command);
}

static bool queue_summary_refresh(market_pair_id_t pair, bool force)
{
    market_command_t command = {
        .type = MARKET_CMD_REFRESH_SUMMARY,
        .pair = pair,
        .interval = MARKET_INTERVAL_1H,
    };
    bool should_publish = false;
    int64_t now_us = esp_timer_get_time();
    int64_t min_interval_us;

    if (!market_pair_is_valid(pair)) {
        return false;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    min_interval_us = summary_refresh_interval_us(s_refresh_mode);
    if (!refresh_mode_allows_network(s_refresh_mode) || s_summary_pending[pair] ||
        (min_interval_us > 0 && !force &&
         (now_us - s_last_summary_request_us[pair]) < min_interval_us)) {
        xSemaphoreGive(s_mutex);
        return false;
    }

    s_summary_pending[pair] = true;
    s_last_summary_request_us[pair] = now_us;
    s_summaries[pair].loading = true;
    s_summaries[pair].had_error = false;
    should_publish = !s_summaries[pair].valid;
    xSemaphoreGive(s_mutex);

    if (!queue_command(&command)) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_summary_pending[pair] = false;
        s_summaries[pair].loading = false;
        xSemaphoreGive(s_mutex);
        return false;
    }

    if (should_publish) {
        publish_market_event();
    }

    return true;
}

static bool queue_chart_refresh(market_pair_id_t pair, market_interval_id_t interval, bool force)
{
    market_command_t command = {
        .type = MARKET_CMD_REFRESH_CHART,
        .pair = pair,
        .interval = interval,
    };
    bool should_publish = false;
    int64_t now_us = esp_timer_get_time();
    int64_t min_interval_us;

    if (!market_pair_is_valid(pair) || !market_interval_is_valid(interval)) {
        return false;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    min_interval_us = chart_refresh_interval_us(s_refresh_mode);
    if (!refresh_mode_allows_network(s_refresh_mode) || s_chart_pending[pair][interval] ||
        (min_interval_us > 0 && !force &&
         (now_us - s_last_chart_request_us[pair][interval]) < min_interval_us)) {
        xSemaphoreGive(s_mutex);
        return false;
    }

    s_chart_pending[pair][interval] = true;
    s_last_chart_request_us[pair][interval] = now_us;
    s_charts[pair][interval].loading = true;
    s_charts[pair][interval].had_error = false;
    should_publish = !s_charts[pair][interval].valid;
    xSemaphoreGive(s_mutex);

    if (!queue_command(&command)) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_chart_pending[pair][interval] = false;
        s_charts[pair][interval].loading = false;
        xSemaphoreGive(s_mutex);
        return false;
    }

    if (should_publish) {
        publish_market_event();
    }

    return true;
}

static void queue_selected_refreshes(bool force)
{
    market_selection_t selection;
    bool stream_active = false;

    if (!net_manager_is_connected()) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    selection = s_selection;
    stream_active = s_stream_connected && s_refresh_mode == REFRESH_MODE_REALTIME &&
                    market_binance_stream_is_connected();
    xSemaphoreGive(s_mutex);

    if (!force && stream_active) {
        return;
    }

    (void)queue_summary_refresh(selection.pair, force);
    (void)queue_chart_refresh(selection.pair, selection.interval, force);
}

static void apply_summary_success(market_pair_id_t pair, const market_feed_summary_t *summary)
{
    market_summary_cache_t *cache = &s_summaries[pair];

    cache->valid = true;
    cache->loading = false;
    cache->had_error = false;
    cache->updated_at_epoch_s = current_epoch_s();
    cache->updated_at_monotonic_us = esp_timer_get_time();
    cache->last_error_epoch_s = 0;
    cache->last_price_scaled = summary->last_price_scaled;
    cache->change_bp = summary->change_bp;
    format_price_text(cache->price_text, sizeof(cache->price_text), pair,
                      summary->last_price_scaled);
    format_change_text(cache->change_text, sizeof(cache->change_text), summary->change_bp);
}

static void apply_summary_failure(market_pair_id_t pair)
{
    market_summary_cache_t *cache = &s_summaries[pair];

    cache->loading = false;
    cache->had_error = true;
    cache->last_error_epoch_s = current_epoch_s();
}

static void apply_chart_success(market_pair_id_t pair, market_interval_id_t interval,
                                const market_candle_window_t *window)
{
    market_chart_cache_t *cache = &s_charts[pair][interval];

    cache->valid = (window->count > 0);
    cache->loading = false;
    cache->had_error = false;
    cache->updated_at_epoch_s = current_epoch_s();
    cache->last_error_epoch_s = 0;
    cache->window = *window;
}

static void apply_chart_failure(market_pair_id_t pair, market_interval_id_t interval)
{
    market_chart_cache_t *cache = &s_charts[pair][interval];

    cache->loading = false;
    cache->had_error = true;
    cache->last_error_epoch_s = current_epoch_s();
}

static void merge_stream_candle_locked(market_pair_id_t pair, market_interval_id_t interval,
                                       const market_candle_t *candle)
{
    market_chart_cache_t *cache = &s_charts[pair][interval];

    if (candle == NULL) {
        return;
    }

    if (!cache->valid || cache->window.count == 0) {
        memset(&cache->window, 0, sizeof(cache->window));
        cache->window.count = 1;
        cache->window.candles[0] = *candle;
    } else {
        const uint16_t last_idx = cache->window.count - 1U;
        const uint32_t last_open_time = cache->window.candles[last_idx].open_time_epoch_s;

        if (candle->open_time_epoch_s < last_open_time) {
            return;
        }
        if (candle->open_time_epoch_s == last_open_time) {
            cache->window.candles[last_idx] = *candle;
        } else if (cache->window.count < MARKET_MAX_CANDLES) {
            cache->window.candles[cache->window.count] = *candle;
            cache->window.count++;
        } else {
            memmove(&cache->window.candles[0], &cache->window.candles[1],
                    sizeof(cache->window.candles[0]) * (MARKET_MAX_CANDLES - 1U));
            cache->window.candles[MARKET_MAX_CANDLES - 1U] = *candle;
        }
    }

    cache->valid = (cache->window.count > 0);
    cache->loading = false;
    cache->had_error = false;
    cache->updated_at_epoch_s = current_epoch_s();
    cache->last_error_epoch_s = 0;
}

static void sync_stream_connection(bool clear_backoff)
{
    market_selection_t selection;
    refresh_mode_t refresh_mode;
    bool should_run;
    int64_t retry_after_us;
    esp_err_t err;
    const bool wifi_connected = net_manager_is_connected();
    const int64_t now_us = esp_timer_get_time();

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    selection = s_selection;
    refresh_mode = s_refresh_mode;
    if (clear_backoff) {
        s_stream_retry_after_us = 0;
    }
    retry_after_us = s_stream_retry_after_us;
    should_run = stream_should_run(refresh_mode, wifi_connected);
    xSemaphoreGive(s_mutex);

    if (!should_run) {
        market_binance_stream_stop();
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_stream_connected = false;
        s_stream_retry_after_us = 0;
        if (s_transport_hint == MARKET_TRANSPORT_STREAMING) {
            s_transport_hint = MARKET_TRANSPORT_POLLING;
            s_active_source = MARKET_SOURCE_GATE;
            s_fallback_active = false;
        }
        xSemaphoreGive(s_mutex);
        return;
    }

    if (retry_after_us > now_us) {
        return;
    }

    err = market_binance_stream_start(selection.pair, selection.interval);
    if (err != ESP_OK) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_stream_retry_after_us = now_us + MARKET_STREAM_RETRY_US;
        s_stream_connected = false;
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "stream start failed for %s %s: %s", market_pair_label(selection.pair),
                 market_interval_label(selection.interval), esp_err_to_name(err));
    }
}

static void market_service_task(void *arg)
{
    market_command_t command;

    (void)arg;
    for (;;) {
        if (xQueueReceive(s_command_queue, &command, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (!net_manager_is_connected() && (command.type == MARKET_CMD_REFRESH_SUMMARY ||
                                            command.type == MARKET_CMD_REFRESH_CHART)) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            if (command.type == MARKET_CMD_REFRESH_SUMMARY) {
                s_summary_pending[command.pair] = false;
                s_summaries[command.pair].loading = false;
            } else if (command.type == MARKET_CMD_REFRESH_CHART) {
                s_chart_pending[command.pair][command.interval] = false;
                s_charts[command.pair][command.interval].loading = false;
            }
            xSemaphoreGive(s_mutex);
            publish_market_event();
            continue;
        }

        if (!net_manager_is_connected()) {
            continue;
        }

        if (command.type == MARKET_CMD_REFRESH_SUMMARY) {
            market_feed_summary_t summary = {0};
            market_source_t used_source = MARKET_SOURCE_UNKNOWN;
            esp_err_t err = fetch_summary_with_fallback(command.pair, &summary, &used_source);

            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_summary_pending[command.pair] = false;
            if (err == ESP_OK) {
                apply_summary_success(command.pair, &summary);
                if (s_stream_connected && command.pair == s_selection.pair &&
                    s_refresh_mode == REFRESH_MODE_REALTIME) {
                    note_streaming_active_locked();
                }
            } else {
                apply_summary_failure(command.pair);
                ESP_LOGW(TAG, "summary refresh failed for %s: %s", market_pair_label(command.pair),
                         esp_err_to_name(err));
            }
            xSemaphoreGive(s_mutex);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "summary source=%s pair=%s", market_source_label(used_source),
                         market_pair_label(command.pair));
            }
            publish_market_event();
            continue;
        }

        if (command.type == MARKET_CMD_REFRESH_CHART) {
            market_candle_window_t window = {0};
            market_source_t used_source = MARKET_SOURCE_UNKNOWN;
            esp_err_t err =
                fetch_candles_with_fallback(command.pair, command.interval, &window, &used_source);

            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_chart_pending[command.pair][command.interval] = false;
            if (err == ESP_OK) {
                apply_chart_success(command.pair, command.interval, &window);
                if (s_stream_connected &&
                    stream_selection_matches_locked(command.pair, command.interval) &&
                    s_refresh_mode == REFRESH_MODE_REALTIME) {
                    note_streaming_active_locked();
                }
            } else {
                apply_chart_failure(command.pair, command.interval);
                ESP_LOGW(TAG, "chart refresh failed for %s %s: %s", market_pair_label(command.pair),
                         market_interval_label(command.interval), esp_err_to_name(err));
            }
            xSemaphoreGive(s_mutex);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "chart source=%s pair=%s interval=%s",
                         market_source_label(used_source), market_pair_label(command.pair),
                         market_interval_label(command.interval));
            }
            publish_market_event();
            continue;
        }

        if (command.type == MARKET_CMD_STREAM_CONNECTED) {
            bool needs_chart_seed = false;

            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_stream_connected = true;
            s_stream_retry_after_us = 0;
            if (stream_selection_matches_locked(command.pair, command.interval) &&
                s_refresh_mode == REFRESH_MODE_REALTIME) {
                note_streaming_active_locked();
                needs_chart_seed = !s_charts[command.pair][command.interval].valid;
            }
            xSemaphoreGive(s_mutex);
            if (needs_chart_seed) {
                (void)queue_chart_refresh(command.pair, command.interval, true);
            }
            publish_market_event();
            continue;
        }

        if (command.type == MARKET_CMD_STREAM_DISCONNECTED) {
            market_binance_stream_stop();
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_stream_connected = false;
            s_stream_retry_after_us = esp_timer_get_time() + MARKET_STREAM_RETRY_US;
            if (s_transport_hint == MARKET_TRANSPORT_STREAMING) {
                s_transport_hint = MARKET_TRANSPORT_POLLING;
                s_active_source = MARKET_SOURCE_GATE;
                s_fallback_active = false;
            }
            xSemaphoreGive(s_mutex);
            ESP_LOGW(TAG, "stream disconnected for %s %s status=%d err=%s",
                     market_pair_label(command.pair), market_interval_label(command.interval),
                     command.status_code, esp_err_to_name(command.error));
            publish_market_event();
            queue_selected_refreshes(true);
            continue;
        }

        if (command.type == MARKET_CMD_STREAM_SUMMARY) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            if (command.pair == s_selection.pair && s_refresh_mode == REFRESH_MODE_REALTIME) {
                s_summary_pending[command.pair] = false;
                apply_summary_success(command.pair, &command.summary);
                note_streaming_active_locked();
            }
            xSemaphoreGive(s_mutex);
            publish_market_event();
            continue;
        }

        if (command.type == MARKET_CMD_STREAM_KLINE) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            if (stream_selection_matches_locked(command.pair, command.interval) &&
                s_refresh_mode == REFRESH_MODE_REALTIME) {
                s_chart_pending[command.pair][command.interval] = false;
                merge_stream_candle_locked(command.pair, command.interval, &command.candle);
                note_streaming_active_locked();
            }
            xSemaphoreGive(s_mutex);
            publish_market_event();
        }
    }
}

static void market_service_event_handler(const app_event_t *event, void *context)
{
    power_policy_output_t policy;
    bool publish = false;

    (void)context;
    if (event == NULL) {
        return;
    }

    switch (event->type) {
    case APP_EVENT_POWER_CHANGED:
        power_policy_get_output(&policy);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (s_refresh_mode != policy.market_mode) {
            s_refresh_mode = policy.market_mode;
            publish = true;
        }
        xSemaphoreGive(s_mutex);
        if (publish) {
            publish_market_event();
        }
        if (refresh_mode_allows_network(policy.market_mode)) {
            queue_selected_refreshes(true);
        }
        sync_stream_connection(true);
        break;
    case APP_EVENT_NET_CHANGED:
        publish_market_event();
        queue_selected_refreshes(true);
        sync_stream_connection(true);
        break;
    case APP_EVENT_TICK_1S:
        sync_stream_connection(false);
        queue_selected_refreshes(false);
        break;
    default:
        break;
    }
}

static trading_data_state_t market_snapshot_state(const market_summary_cache_t *summary,
                                                  bool wifi_connected)
{
    int64_t age_us;
    int64_t now_us;

    if (summary == NULL) {
        return TRADING_DATA_EMPTY;
    }

    if (summary->valid) {
        now_us = esp_timer_get_time();
        if (!wifi_connected || summary->updated_at_monotonic_us <= 0) {
            return TRADING_DATA_STALE;
        }

        age_us = now_us - summary->updated_at_monotonic_us;
        if (age_us > MARKET_SUMMARY_STALE_US || summary->had_error) {
            return TRADING_DATA_STALE;
        }

        return TRADING_DATA_LIVE;
    }

    if (summary->loading) {
        return TRADING_DATA_LOADING;
    }

    if (summary->had_error) {
        return TRADING_DATA_ERROR;
    }

    return TRADING_DATA_EMPTY;
}

esp_err_t market_service_init(void)
{
    power_policy_output_t policy;
    const market_binance_stream_callbacks_t stream_callbacks = {
        .on_connected = market_stream_connected_cb,
        .on_disconnected = market_stream_disconnected_cb,
        .on_summary = market_stream_summary_cb,
        .on_kline = market_stream_kline_cb,
    };

    s_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_mutex != NULL, ESP_ERR_NO_MEM, TAG, "market mutex alloc failed");

    s_command_queue = xQueueCreate(MARKET_SERVICE_QUEUE_LEN, sizeof(market_command_t));
    ESP_RETURN_ON_FALSE(s_command_queue != NULL, ESP_ERR_NO_MEM, TAG, "market queue alloc failed");

    memset(s_summaries, 0, sizeof(s_summaries));
    memset(s_charts, 0, sizeof(s_charts));
    memset(s_last_summary_request_us, 0, sizeof(s_last_summary_request_us));
    memset(s_last_chart_request_us, 0, sizeof(s_last_chart_request_us));
    memset(s_summary_pending, 0, sizeof(s_summary_pending));
    memset(s_chart_pending, 0, sizeof(s_chart_pending));

    s_feeds[MARKET_SOURCE_GATE] = market_feed_gate_get_iface();
    s_feeds[MARKET_SOURCE_BINANCE] = market_feed_binance_get_iface();
    ESP_RETURN_ON_FALSE(s_feeds[MARKET_SOURCE_GATE] != NULL &&
                            s_feeds[MARKET_SOURCE_BINANCE] != NULL,
                        ESP_ERR_INVALID_STATE, TAG, "market feeds missing");

    s_active_source = MARKET_SOURCE_GATE;
    s_transport_hint = s_feeds[MARKET_SOURCE_GATE]->transport_hint();
    s_fallback_active = false;
    s_stream_connected = false;
    s_source_error_count = 0;
    s_primary_retry_after_us = 0;
    s_stream_retry_after_us = 0;
    s_selection.pair = MARKET_PAIR_BTC_USDT;
    s_selection.interval = MARKET_INTERVAL_1H;
    power_policy_get_output(&policy);
    s_refresh_mode = policy.market_mode;

    ESP_RETURN_ON_ERROR(market_binance_stream_init(&stream_callbacks, NULL), TAG,
                        "stream init failed");
    ESP_RETURN_ON_ERROR(event_bus_subscribe(market_service_event_handler, NULL), TAG,
                        "market subscribe failed");
    {
        BaseType_t ret = xTaskCreatePinnedToCore(market_service_task, "market_service",
                                                 MARKET_SERVICE_TASK_STACK, NULL, 4, NULL, 1);
        ESP_RETURN_ON_FALSE(ret == pdPASS, ESP_ERR_NO_MEM, TAG, "market task create failed");
    }

    return ESP_OK;
}

void market_service_select_pair(market_pair_id_t pair)
{
    bool changed = false;

    if (!market_pair_is_valid(pair)) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_selection.pair != pair) {
        s_selection.pair = pair;
        changed = true;
    }
    xSemaphoreGive(s_mutex);

    if (!changed) {
        return;
    }

    publish_market_event();
    queue_selected_refreshes(true);
    sync_stream_connection(true);
}

void market_service_select_interval(market_interval_id_t interval)
{
    bool changed = false;

    if (!market_interval_is_valid(interval)) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_selection.interval != interval) {
        s_selection.interval = interval;
        changed = true;
    }
    xSemaphoreGive(s_mutex);

    if (!changed) {
        return;
    }

    publish_market_event();
    queue_selected_refreshes(true);
    sync_stream_connection(true);
}

void market_service_get_snapshot(market_snapshot_t *out)
{
    market_selection_t selection;
    market_summary_cache_t summary;
    market_chart_cache_t chart;
    bool wifi_connected;

    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    selection = s_selection;
    summary = s_summaries[selection.pair];
    chart = s_charts[selection.pair][selection.interval];
    out->transport_hint = s_transport_hint;
    out->active_source = s_active_source;
    out->fallback_active = s_fallback_active;
    out->source_error_count = s_source_error_count;
    xSemaphoreGive(s_mutex);

    wifi_connected = net_manager_is_connected();
    out->selection = selection;
    out->wifi_connected = wifi_connected;
    out->summary_updated_at_epoch_s = summary.updated_at_epoch_s;
    out->chart_updated_at_epoch_s = chart.updated_at_epoch_s;
    out->change_bp = summary.change_bp;
    out->last_price_scaled = summary.last_price_scaled;
    out->state = market_snapshot_state(&summary, wifi_connected);
    out->has_chart_data = chart.valid && chart.window.count > 0;
    out->candle_count = out->has_chart_data ? chart.window.count : 0;
    snprintf(out->pair_label, sizeof(out->pair_label), "%s", market_pair_label(selection.pair));

    if (summary.valid) {
        snprintf(out->price_text, sizeof(out->price_text), "%s", summary.price_text);
        snprintf(out->change_text, sizeof(out->change_text), "%s", summary.change_text);
    } else {
        snprintf(out->price_text, sizeof(out->price_text), "%s", "--");
        snprintf(out->change_text, sizeof(out->change_text), "%s", "--");
    }
}

bool market_service_has_chart_data(market_pair_id_t pair, market_interval_id_t interval)
{
    bool has_data = false;

    if (!market_pair_is_valid(pair) || !market_interval_is_valid(interval)) {
        return false;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    has_data = s_charts[pair][interval].valid && s_charts[pair][interval].window.count > 0;
    xSemaphoreGive(s_mutex);
    return has_data;
}

bool market_service_get_candles(market_pair_id_t pair, market_interval_id_t interval,
                                market_candle_window_t *out)
{
    bool has_data = false;

    if (!market_pair_is_valid(pair) || !market_interval_is_valid(interval) || out == NULL) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_charts[pair][interval].valid) {
        *out = s_charts[pair][interval].window;
        has_data = (out->count > 0);
    }
    xSemaphoreGive(s_mutex);
    return has_data;
}
