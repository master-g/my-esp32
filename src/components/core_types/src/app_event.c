#include "core_types/app_event.h"

const char *app_event_type_to_string(app_event_type_t type)
{
    switch (type) {
    case APP_EVENT_ENTER:
        return "enter";
    case APP_EVENT_LEAVE:
        return "leave";
    case APP_EVENT_TICK_1S:
        return "tick_1s";
    case APP_EVENT_TOUCH:
        return "touch";
    case APP_EVENT_NET_CHANGED:
        return "net_changed";
    case APP_EVENT_POWER_CHANGED:
        return "power_changed";
    case APP_EVENT_DATA_CLAUDE:
        return "data_claude";
    case APP_EVENT_DATA_MARKET:
        return "data_market";
    case APP_EVENT_DATA_WEATHER:
        return "data_weather";
    case APP_EVENT_DATA_BITCOIN:
        return "data_bitcoin";
    case APP_EVENT_PERMISSION_REQUEST:
        return "permission_request";
    default:
        return "unknown";
    }
}
