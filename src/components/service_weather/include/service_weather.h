#ifndef SERVICE_WEATHER_H
#define SERVICE_WEATHER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define WEATHER_CITY_LABEL_MAX 24
#define WEATHER_COORD_TEXT_MAX 24

typedef enum {
    WEATHER_EMPTY = 0,
    WEATHER_LIVE,
    WEATHER_STALE,
    WEATHER_REFRESHING,
    WEATHER_ERROR,
} weather_state_t;

typedef enum {
    WEATHER_ICON_UNKNOWN = 0,
    WEATHER_ICON_CLEAR_DAY,
    WEATHER_ICON_CLEAR_NIGHT,
    WEATHER_ICON_PARTLY_CLOUDY_DAY,
    WEATHER_ICON_PARTLY_CLOUDY_NIGHT,
    WEATHER_ICON_CLOUDY,
    WEATHER_ICON_FOG,
    WEATHER_ICON_DRIZZLE,
    WEATHER_ICON_RAIN,
    WEATHER_ICON_HEAVY_RAIN,
    WEATHER_ICON_SNOW,
    WEATHER_ICON_THUNDER,
} weather_icon_t;

typedef struct {
    char city_label[WEATHER_CITY_LABEL_MAX];
    char latitude[WEATHER_COORD_TEXT_MAX];
    char longitude[WEATHER_COORD_TEXT_MAX];
} weather_location_config_t;

typedef struct {
    weather_state_t state;
    uint32_t updated_at_epoch_s;
    uint32_t last_error_epoch_s;
    char city[WEATHER_CITY_LABEL_MAX];
    char text[24];
    int16_t temperature_c_tenths;
    weather_icon_t icon_id;
} weather_snapshot_t;

esp_err_t weather_service_init(void);
void weather_service_request_refresh(void);
bool weather_service_can_refresh(void);
void weather_service_get_location_config(weather_location_config_t *config);
esp_err_t weather_service_apply_location_config(const weather_location_config_t *config);
void weather_service_get_snapshot(weather_snapshot_t *out);

#endif
