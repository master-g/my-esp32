#include "service_weather.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"

static weather_snapshot_t s_snapshot;
static int64_t s_last_request_us;

esp_err_t weather_service_init(void)
{
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.state = WEATHER_EMPTY;
    s_last_request_us = 0;
    return ESP_OK;
}

void weather_service_request_refresh(void)
{
    if (!weather_service_can_refresh()) {
        return;
    }

    s_last_request_us = esp_timer_get_time();
    s_snapshot.state = WEATHER_REFRESHING;
}

bool weather_service_can_refresh(void)
{
    if (s_last_request_us == 0) {
        return true;
    }

    return (esp_timer_get_time() - s_last_request_us) >= 60000000LL;
}

const weather_snapshot_t *weather_service_get_snapshot(void)
{
    return &s_snapshot;
}
