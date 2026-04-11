#ifndef CORE_TYPES_APP_ID_H
#define CORE_TYPES_APP_ID_H

#include <stdint.h>

typedef enum {
    APP_ID_HOME = 0,
    APP_ID_TRADING,
    APP_ID_SATOSHI_SLOT,
    APP_ID_SETTINGS,
    APP_ID_COUNT,
    APP_ID_INVALID = 0xff,
} app_id_t;

const char *app_id_to_string(app_id_t app_id);

#endif
