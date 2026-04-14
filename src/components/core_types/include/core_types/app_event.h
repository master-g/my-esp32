#ifndef CORE_TYPES_APP_EVENT_H
#define CORE_TYPES_APP_EVENT_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    APP_EVENT_ENTER = 0,
    APP_EVENT_LEAVE,
    APP_EVENT_TICK_1S,
    APP_EVENT_TOUCH,
    APP_EVENT_NET_CHANGED,
    APP_EVENT_POWER_CHANGED,
    APP_EVENT_DATA_CLAUDE,
    APP_EVENT_DATA_MARKET,
    APP_EVENT_DATA_WEATHER,
    APP_EVENT_DATA_SETTINGS,
    APP_EVENT_PERMISSION_REQUEST,
    APP_EVENT_PERMISSION_DISMISS,
    APP_EVENT_PROMPT_REQUEST,
    APP_EVENT_PROMPT_DISMISS,
} app_event_type_t;

typedef enum {
    APP_TOUCH_EDGE_NONE = 0,
    APP_TOUCH_EDGE_LEFT,
    APP_TOUCH_EDGE_RIGHT,
} app_touch_edge_t;

typedef enum {
    APP_TOUCH_SWIPE_NONE = 0,
    APP_TOUCH_SWIPE_LEFT,
    APP_TOUCH_SWIPE_RIGHT,
} app_touch_swipe_t;

typedef struct {
    app_touch_edge_t edge;
    app_touch_swipe_t swipe;
    int16_t delta_x;
    int16_t delta_y;
    uint16_t start_x;
    uint16_t start_y;
} app_touch_event_t;

typedef struct {
    app_event_type_t type;
    const void *payload;
} app_event_t;

const char *app_event_type_to_string(app_event_type_t type);

#endif
