#include "trading_presenter.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ui_theme.h"

#define TRADING_COLOR_TEXT ui_theme_color_hex(UI_THEME_COLOR_TEXT_PRIMARY)
#define TRADING_COLOR_MUTED ui_theme_color_hex(UI_THEME_COLOR_TEXT_MUTED)
#define TRADING_COLOR_UP ui_theme_color_hex(UI_THEME_COLOR_STATUS_SUCCESS)
#define TRADING_COLOR_DOWN ui_theme_color_hex(UI_THEME_COLOR_STATUS_ERROR)
#define TRADING_COLOR_BINANCE_UP 0x0ECB81
#define TRADING_COLOR_BINANCE_DOWN 0xF6465D
#define TRADING_COLOR_STALE ui_theme_color_hex(UI_THEME_COLOR_STATUS_WARNING)
#define TRADING_COLOR_ERROR ui_theme_color_hex(UI_THEME_COLOR_STATUS_ERROR)
#define TRADING_COLOR_LOADING ui_theme_color_hex(UI_THEME_COLOR_STATUS_INFO)

static const char *source_chip_label(market_source_t source)
{
    switch (source) {
    case MARKET_SOURCE_BINANCE:
        return "BNCE";
    case MARKET_SOURCE_GATE:
        return "GATE";
    default:
        return "--";
    }
}

static void format_status_time(char *out, size_t out_size, uint32_t epoch_s, const char *prefix)
{
    struct tm now_tm = {0};
    time_t now = (time_t)epoch_s;

    if (out == NULL || out_size == 0) {
        return;
    }

    if (epoch_s == 0 || prefix == NULL) {
        out[0] = '\0';
        return;
    }

    localtime_r(&now, &now_tm);
    snprintf(out, out_size, "%s %02d:%02d:%02d", prefix, now_tm.tm_hour, now_tm.tm_min,
             now_tm.tm_sec);
}

static void append_interval_suffix(char *out, size_t out_size, market_interval_id_t interval)
{
    size_t used;

    if (out == NULL || out_size == 0 || out[0] == '\0') {
        return;
    }

    used = strlen(out);
    if (used >= out_size - 1) {
        return;
    }

    snprintf(out + used, out_size - used, " · %s", market_interval_label(interval));
}

void trading_presenter_build(trading_present_model_t *out, const market_snapshot_t *snapshot,
                             const market_candle_window_t *candles, trading_price_tick_t price_tick)
{
    size_t i = 0;

    if (out == NULL || snapshot == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));
    snprintf(out->price_text, sizeof(out->price_text), "%s", snapshot->price_text);
    snprintf(out->change_text, sizeof(out->change_text), "%s", snapshot->change_text);
    out->price_color = TRADING_COLOR_TEXT;
    out->change_color = TRADING_COLOR_MUTED;
    out->status_color = TRADING_COLOR_MUTED;
    out->transport_color = TRADING_COLOR_MUTED;
    out->chart_dimmed = false;
    out->fallback_active = snapshot->fallback_active;

    for (i = 0; i < MARKET_PAIR_COUNT; ++i) {
        out->pair_selected[i] = (snapshot->selection.pair == (market_pair_id_t)i);
    }

    snprintf(out->transport_text, sizeof(out->transport_text), "%s",
             source_chip_label(snapshot->active_source));
    if (snapshot->fallback_active) {
        out->transport_color = TRADING_COLOR_STALE;
    }

    if (snapshot->change_text[0] == '+' || snapshot->change_bp > 0) {
        out->change_color = TRADING_COLOR_UP;
    } else if (snapshot->change_text[0] == '-' || snapshot->change_bp < 0) {
        out->change_color = TRADING_COLOR_DOWN;
    }

    if (snapshot->binance_price_colors) {
        if (price_tick == TRADING_PRICE_TICK_UP) {
            out->price_color = TRADING_COLOR_BINANCE_UP;
        } else if (price_tick == TRADING_PRICE_TICK_DOWN) {
            out->price_color = TRADING_COLOR_BINANCE_DOWN;
        }
    }

    switch (snapshot->state) {
    case TRADING_DATA_LIVE:
        format_status_time(out->status_text, sizeof(out->status_text),
                           snapshot->summary_updated_at_epoch_s, "LIVE");
        if (out->status_text[0] == '\0') {
            snprintf(out->status_text, sizeof(out->status_text), "%s", "LIVE");
        }
        out->status_color = TRADING_COLOR_UP;
        if (snapshot->fallback_active) {
            snprintf(out->status_text, sizeof(out->status_text), "LIVE %s fallback",
                     source_chip_label(snapshot->active_source));
            out->status_color = TRADING_COLOR_STALE;
        }
        append_interval_suffix(out->status_text, sizeof(out->status_text),
                               snapshot->selection.interval);
        break;
    case TRADING_DATA_STALE:
        format_status_time(out->status_text, sizeof(out->status_text),
                           snapshot->summary_updated_at_epoch_s, "CACHED");
        if (out->status_text[0] == '\0') {
            snprintf(out->status_text, sizeof(out->status_text), "%s", "CACHED");
        }
        if (snapshot->fallback_active) {
            snprintf(out->status_text, sizeof(out->status_text), "CACHED %s fallback",
                     source_chip_label(snapshot->active_source));
        }
        out->status_color = TRADING_COLOR_STALE;
        out->chart_dimmed = true;
        append_interval_suffix(out->status_text, sizeof(out->status_text),
                               snapshot->selection.interval);
        break;
    case TRADING_DATA_LOADING:
        snprintf(out->status_text, sizeof(out->status_text), "%s", "LOADING MARKET FEED");
        out->status_color = TRADING_COLOR_LOADING;
        out->price_color = TRADING_COLOR_MUTED;
        append_interval_suffix(out->status_text, sizeof(out->status_text),
                               snapshot->selection.interval);
        break;
    case TRADING_DATA_ERROR:
        snprintf(out->status_text, sizeof(out->status_text), "%s", "FEED UNAVAILABLE");
        out->status_color = TRADING_COLOR_ERROR;
        out->price_color = TRADING_COLOR_MUTED;
        out->chart_dimmed = true;
        append_interval_suffix(out->status_text, sizeof(out->status_text),
                               snapshot->selection.interval);
        break;
    case TRADING_DATA_EMPTY:
    default:
        if (snapshot->wifi_connected) {
            snprintf(out->status_text, sizeof(out->status_text), "%s", "WAITING FOR FIRST TICK");
        } else {
            snprintf(out->status_text, sizeof(out->status_text), "%s", "OFFLINE");
        }
        out->status_color = TRADING_COLOR_MUTED;
        out->price_color = TRADING_COLOR_MUTED;
        append_interval_suffix(out->status_text, sizeof(out->status_text),
                               snapshot->selection.interval);
        break;
    }

    if (snapshot->has_chart_data && candles != NULL && candles->count > 0) {
        out->candles = *candles;
        out->has_chart_data = true;
    } else if (!snapshot->wifi_connected) {
        snprintf(out->chart_status_text, sizeof(out->chart_status_text), "%s",
                 "No chart while offline");
    } else if (snapshot->state == TRADING_DATA_LOADING) {
        snprintf(out->chart_status_text, sizeof(out->chart_status_text), "%s", "Loading candles");
    } else if (snapshot->state == TRADING_DATA_ERROR) {
        snprintf(out->chart_status_text, sizeof(out->chart_status_text), "%s", "Chart unavailable");
    } else {
        snprintf(out->chart_status_text, sizeof(out->chart_status_text), "%s",
                 "Waiting for chart cache");
    }
}
