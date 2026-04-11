#include "home_presenter.h"

#include <stdio.h>
#include <string.h>

#include "generated/app_home_status_font.h"
#include "home_internal.h"

static void display_city_name(char *dst, size_t dst_size, const char *src)
{
    size_t len = 0;

    if (dst == NULL || dst_size == 0) {
        return;
    }
    if (src == NULL || src[0] == '\0') {
        snprintf(dst, dst_size, "%s", "--");
        return;
    }

    while (src[len] != '\0' && src[len] != ',' && len + 1 < dst_size) {
        dst[len] = src[len];
        len++;
    }
    while (len > 0 && dst[len - 1] == ' ') {
        len--;
    }
    dst[len] = '\0';
}

static int rounded_temperature_c(int16_t temperature_c_tenths)
{
    if (temperature_c_tenths >= 0) {
        return (temperature_c_tenths + 5) / 10;
    }

    return (temperature_c_tenths - 5) / 10;
}

static bool home_snapshot_has_weather(const home_snapshot_t *snapshot)
{
    return snapshot != NULL && snapshot->weather_available && snapshot->updated_at_epoch_s != 0;
}

static const char *weather_icon_symbol(weather_icon_t icon)
{
    switch (icon) {
    case WEATHER_ICON_CLEAR_DAY:
        return APP_HOME_SYMBOL_WEATHER_CLEAR_DAY;
    case WEATHER_ICON_CLEAR_NIGHT:
        return APP_HOME_SYMBOL_WEATHER_CLEAR_NIGHT;
    case WEATHER_ICON_PARTLY_CLOUDY_DAY:
        return APP_HOME_SYMBOL_WEATHER_PARTLY_CLOUDY_DAY;
    case WEATHER_ICON_PARTLY_CLOUDY_NIGHT:
        return APP_HOME_SYMBOL_WEATHER_PARTLY_CLOUDY_NIGHT;
    case WEATHER_ICON_CLOUDY:
        return APP_HOME_SYMBOL_WEATHER_CLOUDY;
    case WEATHER_ICON_FOG:
        return APP_HOME_SYMBOL_WEATHER_FOG;
    case WEATHER_ICON_DRIZZLE:
        return APP_HOME_SYMBOL_WEATHER_DRIZZLE;
    case WEATHER_ICON_RAIN:
        return APP_HOME_SYMBOL_WEATHER_RAIN;
    case WEATHER_ICON_HEAVY_RAIN:
        return APP_HOME_SYMBOL_WEATHER_HEAVY_RAIN;
    case WEATHER_ICON_SNOW:
        return APP_HOME_SYMBOL_WEATHER_SNOW;
    case WEATHER_ICON_THUNDER:
        return APP_HOME_SYMBOL_WEATHER_THUNDER;
    case WEATHER_ICON_UNKNOWN:
    default:
        return APP_HOME_SYMBOL_WEATHER_UNKNOWN;
    }
}

static sprite_state_t map_run_state(claude_run_state_t rs, bool connected)
{
    if (!connected) {
        return SPRITE_STATE_SLEEPING;
    }

    switch (rs) {
    case CLAUDE_RUN_PROCESSING:
    case CLAUDE_RUN_RUNNING_TOOL:
    case CLAUDE_RUN_COMPACTING:
        return SPRITE_STATE_WORKING;
    case CLAUDE_RUN_WAITING_FOR_INPUT:
        return SPRITE_STATE_WAITING;
    case CLAUDE_RUN_ENDED:
        return SPRITE_STATE_SLEEPING;
    default:
        return SPRITE_STATE_IDLE;
    }
}

void home_presenter_build(home_present_model_t *out, const home_snapshot_t *snapshot)
{
    bool has_weather;
    char city_name[24];

    if (out == NULL || snapshot == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));

    snprintf(out->time_text, sizeof(out->time_text), "%s", snapshot->time_text);
    snprintf(out->date_line, sizeof(out->date_line), "%s  %s", snapshot->date_text,
             snapshot->weekday_text);

    if (snapshot->wifi_connected) {
        out->wifi_symbol = APP_HOME_SYMBOL_WIFI;
        out->wifi_color = HOME_WIFI_ONLINE_COLOR;
    } else if (snapshot->wifi_connecting) {
        out->wifi_symbol = APP_HOME_SYMBOL_WIFI_1;
        out->wifi_color = HOME_WIFI_CONNECTING_COLOR;
        out->wifi_dot_visible = true;
    } else {
        out->wifi_symbol = APP_HOME_SYMBOL_WIFI_OFF;
        out->wifi_color = HOME_ICON_MUTED_COLOR;
    }

    out->claude_color = snapshot->claude_connected || snapshot->claude_unread
                            ? HOME_CLAUDE_ACTIVE_COLOR
                            : HOME_ICON_MUTED_COLOR;
    out->claude_dot_visible = snapshot->claude_unread;

    has_weather = home_snapshot_has_weather(snapshot);
    out->weather_color = has_weather ? HOME_WEATHER_TEXT_COLOR : HOME_WEATHER_MUTED_COLOR;
    out->weather_symbol =
        weather_icon_symbol(has_weather ? snapshot->weather_icon_id : WEATHER_ICON_UNKNOWN);

    display_city_name(city_name, sizeof(city_name), snapshot->city_text);
    if (has_weather) {
        snprintf(out->weather_text, sizeof(out->weather_text), "%s  %d°C", city_name,
                 rounded_temperature_c(snapshot->temperature_c_tenths));
    } else {
        snprintf(out->weather_text, sizeof(out->weather_text), "%s  --", city_name);
    }
    if (snapshot->weather_stale) {
        snprintf(out->weather_text + strlen(out->weather_text),
                 sizeof(out->weather_text) - strlen(out->weather_text), "  cached");
    }

    out->sprite_state = map_run_state(snapshot->claude_run_state, snapshot->claude_connected);
    out->bubble_visible =
        snapshot->claude_detail[0] != '\0' && out->sprite_state != SPRITE_STATE_SLEEPING;
    snprintf(out->bubble_text, sizeof(out->bubble_text), "%s", snapshot->claude_detail);

    if (snapshot->time_text[0] != '\0' && snapshot->time_text[0] != '-') {
        memcpy(out->screensaver_time_text, snapshot->time_text, 5);
        out->screensaver_time_text[5] = '\0';
    } else {
        snprintf(out->screensaver_time_text, sizeof(out->screensaver_time_text), "%s", "--:--");
    }
}
