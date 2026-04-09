#include "service_home.h"

#include <stdio.h>
#include <string.h>

#include "net_manager.h"
#include "service_claude.h"
#include "service_time.h"
#include "service_weather.h"

static home_snapshot_t s_snapshot;

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

esp_err_t home_service_init(void)
{
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    home_service_refresh_snapshot();
    return ESP_OK;
}

void home_service_refresh_snapshot(void)
{
    time_snapshot_t time_snap;
    weather_snapshot_t weather_snap;
    claude_snapshot_t claude_snap;
    net_snapshot_t net_snap;

    time_service_get_snapshot(&time_snap);
    weather_service_get_snapshot(&weather_snap);
    claude_service_get_snapshot(&claude_snap);
    net_manager_get_snapshot(&net_snap);

    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.rtc_valid = time_snap.rtc_valid;
    s_snapshot.ntp_synced = time_snap.ntp_synced;
    s_snapshot.wifi_connected = net_snap.wifi_connected;
    s_snapshot.wifi_connecting = (net_snap.state == NET_STATE_CONNECTING);
    copy_text(s_snapshot.time_text, sizeof(s_snapshot.time_text), time_snap.time_text);
    copy_text(s_snapshot.date_text, sizeof(s_snapshot.date_text), time_snap.date_text);
    copy_text(s_snapshot.weekday_text, sizeof(s_snapshot.weekday_text), time_snap.weekday_text);

    s_snapshot.weather_available = (weather_snap.state == WEATHER_LIVE) ||
                                   (weather_snap.state == WEATHER_STALE) ||
                                   (weather_snap.state == WEATHER_REFRESHING);
    s_snapshot.weather_stale = (weather_snap.state == WEATHER_STALE);
    s_snapshot.updated_at_epoch_s = weather_snap.updated_at_epoch_s;
    copy_text(s_snapshot.weather_text, sizeof(s_snapshot.weather_text), weather_snap.text);
    copy_text(s_snapshot.city_text, sizeof(s_snapshot.city_text), weather_snap.city);
    s_snapshot.temperature_c_tenths = weather_snap.temperature_c_tenths;
    s_snapshot.weather_icon_id = weather_snap.icon_id;
    s_snapshot.claude_connected = (claude_snap.conn_state == CLAUDE_CONN_CONNECTED);
    s_snapshot.claude_unread = claude_snap.unread;
    s_snapshot.claude_run_state = claude_snap.run_state;
}

void home_service_get_snapshot(home_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }

    *out = s_snapshot;
}

void home_service_request_weather_refresh(void)
{
    weather_service_request_refresh();
    home_service_refresh_snapshot();
}

bool home_service_can_refresh_weather(void) { return weather_service_can_refresh(); }
