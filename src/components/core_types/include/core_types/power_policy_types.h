#ifndef CORE_TYPES_POWER_POLICY_TYPES_H
#define CORE_TYPES_POWER_POLICY_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#include "core_types/app_id.h"

/* The board runs from USB with the screen always on: there is no battery, dim
 * or sleep state. The policy only decides how hard each service may work for
 * the app in the foreground. */
typedef enum {
    REFRESH_MODE_REALTIME = 0,
    REFRESH_MODE_BACKGROUND_CACHE,
} refresh_mode_t;

typedef struct {
    app_id_t foreground_app;
    bool wifi_connected;
} power_policy_input_t;

typedef struct {
    uint8_t brightness_percent;
    refresh_mode_t market_mode;
} power_policy_output_t;

#endif
