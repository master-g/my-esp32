#include "service_home.h"

#include <stdio.h>
#include <string.h>

#include "net_manager.h"
#include "service_claude.h"
#include "service_time.h"
#include "service_weather.h"

static void copy_text(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0) {
        return;
    }

    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    snprintf(dst, dst_size, "%s", src);
}

esp_err_t home_service_init(void) { return ESP_OK; }

void home_service_get_snapshot(home_snapshot_t *out)
{
    time_snapshot_t time_snap;
    weather_snapshot_t weather_snap;
    claude_snapshot_t claude_snap;
    net_snapshot_t net_snap;

    if (out == NULL) {
        return;
    }

    time_service_get_snapshot(&time_snap);
    weather_service_get_snapshot(&weather_snap);
    claude_service_get_snapshot(&claude_snap);
    net_manager_get_snapshot(&net_snap);

    memset(out, 0, sizeof(*out));
    out->rtc_valid = time_snap.rtc_valid;
    out->ntp_synced = time_snap.ntp_synced;
    out->wifi_connected = net_snap.wifi_connected;
    out->wifi_connecting = (net_snap.state == NET_STATE_CONNECTING);
    copy_text(out->time_text, sizeof(out->time_text), time_snap.time_text);
    copy_text(out->date_text, sizeof(out->date_text), time_snap.date_text);
    copy_text(out->weekday_text, sizeof(out->weekday_text), time_snap.weekday_text);

    out->weather_available = (weather_snap.state == WEATHER_LIVE) ||
                             (weather_snap.state == WEATHER_STALE) ||
                             (weather_snap.state == WEATHER_REFRESHING);
    out->weather_stale = (weather_snap.state == WEATHER_STALE);
    out->updated_at_epoch_s = weather_snap.updated_at_epoch_s;
    copy_text(out->weather_text, sizeof(out->weather_text), weather_snap.text);
    copy_text(out->city_text, sizeof(out->city_text), weather_snap.city);
    out->temperature_c_tenths = weather_snap.temperature_c_tenths;
    out->weather_icon_id = weather_snap.icon_id;
    out->claude_connected = (claude_snap.conn_state == CLAUDE_CONN_CONNECTED);
    out->claude_unread = claude_snap.unread;
    out->claude_run_state = claude_snap.run_state;
    copy_text(out->claude_detail, sizeof(out->claude_detail), claude_snap.detail);
    copy_text(out->claude_emotion, sizeof(out->claude_emotion), claude_snap.emotion);
}

void home_service_request_weather_refresh(void) { weather_service_request_refresh(); }

bool home_service_can_refresh_weather(void) { return weather_service_can_refresh(); }
