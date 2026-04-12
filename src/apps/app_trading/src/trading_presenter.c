#include "trading_presenter.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ui_theme.h"

#define TRADING_COLOR_TEXT ui_theme_color_hex(UI_THEME_COLOR_TEXT_PRIMARY)
#define TRADING_COLOR_MUTED ui_theme_color_hex(UI_THEME_COLOR_TEXT_MUTED)
#define TRADING_COLOR_UP ui_theme_color_hex(UI_THEME_COLOR_STATUS_SUCCESS)
#define TRADING_COLOR_DOWN ui_theme_color_hex(UI_THEME_COLOR_STATUS_ERROR)
#define TRADING_COLOR_STALE ui_theme_color_hex(UI_THEME_COLOR_STATUS_WARNING)
#define TRADING_COLOR_ERROR ui_theme_color_hex(UI_THEME_COLOR_STATUS_ERROR)
#define TRADING_COLOR_LOADING ui_theme_color_hex(UI_THEME_COLOR_STATUS_INFO)

static const char *source_chip_label(market_source_t source)
{
    switch (source) {
    case MARKET_SOURCE_BINANCE:
        return "BNCE";
    case MARKET_SOURCE_OKX:
        return "OKX";
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

void trading_presenter_build(trading_present_model_t *out, const market_snapshot_t *snapshot,
                             const market_candle_window_t *candles)
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

    for (i = 0; i < MARKET_INTERVAL_COUNT; ++i) {
        out->interval_selected[i] = (snapshot->selection.interval == (market_interval_id_t)i);
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

    switch (snapshot->state) {
    case TRADING_DATA_LIVE:
        format_status_time(out->status_text, sizeof(out->status_text),
                           snapshot->summary_updated_at_epoch_s, "LIVE");
        out->status_color = TRADING_COLOR_UP;
        if (snapshot->fallback_active) {
            snprintf(out->status_text, sizeof(out->status_text), "LIVE %s fallback",
                     source_chip_label(snapshot->active_source));
            out->status_color = TRADING_COLOR_STALE;
        }
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
        break;
    case TRADING_DATA_LOADING:
        snprintf(out->status_text, sizeof(out->status_text), "%s", "LOADING MARKET FEED");
        out->status_color = TRADING_COLOR_LOADING;
        out->price_color = TRADING_COLOR_MUTED;
        break;
    case TRADING_DATA_ERROR:
        snprintf(out->status_text, sizeof(out->status_text), "%s", "FEED UNAVAILABLE");
        out->status_color = TRADING_COLOR_ERROR;
        out->price_color = TRADING_COLOR_MUTED;
        out->chart_dimmed = true;
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
