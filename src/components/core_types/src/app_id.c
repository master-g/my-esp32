#include "core_types/app_id.h"

const char *app_id_to_string(app_id_t app_id)
{
    switch (app_id) {
    case APP_ID_HOME:
        return "home";
    case APP_ID_TRADING:
        return "trading";
    case APP_ID_SATOSHI_SLOT:
        return "satoshi_slot";
    case APP_ID_SETTINGS:
        return "settings";
    case APP_ID_COUNT:
    case APP_ID_INVALID:
    default:
        return "invalid";
    }
}
