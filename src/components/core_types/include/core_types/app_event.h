#ifndef CORE_TYPES_APP_EVENT_H
#define CORE_TYPES_APP_EVENT_H

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
    APP_EVENT_DATA_BITCOIN,
    APP_EVENT_PERMISSION_REQUEST,
    APP_EVENT_PERMISSION_DISMISS,
} app_event_type_t;

typedef struct {
    app_event_type_t type;
    const void *payload;
} app_event_t;

const char *app_event_type_to_string(app_event_type_t type);

#endif
