#ifndef SERVICE_WEATHER_H
#define SERVICE_WEATHER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    WEATHER_EMPTY = 0,
    WEATHER_LIVE,
    WEATHER_STALE,
    WEATHER_REFRESHING,
    WEATHER_ERROR,
} weather_state_t;

typedef struct {
    weather_state_t state;
    uint32_t updated_at_epoch_s;
    char city[24];
    char text[24];
    int16_t temperature_c_tenths;
    uint8_t icon_id;
} weather_snapshot_t;

esp_err_t weather_service_init(void);
void weather_service_request_refresh(void);
bool weather_service_can_refresh(void);
const weather_snapshot_t *weather_service_get_snapshot(void);

#endif
