#ifndef SERVICE_HOME_H
#define SERVICE_HOME_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "service_claude.h"
#include "service_weather.h"

typedef struct {
    bool rtc_valid;
    bool ntp_synced;
    bool wifi_connected;
    bool wifi_connecting;
    bool weather_available;
    bool weather_stale;
    uint32_t updated_at_epoch_s;
    char time_text[9];
    char date_text[24];
    char weekday_text[12];
    char weather_text[24];
    char city_text[24];
    int16_t temperature_c_tenths;
    weather_icon_t weather_icon_id;
    bool claude_connected;
    bool claude_unread;
    claude_run_state_t claude_run_state;
} home_snapshot_t;

esp_err_t home_service_init(void);
void home_service_refresh_snapshot(void);
void home_service_get_snapshot(home_snapshot_t *out);
void home_service_request_weather_refresh(void);
bool home_service_can_refresh_weather(void);

#endif
