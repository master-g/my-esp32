#ifndef CORE_TYPES_POWER_POLICY_TYPES_H
#define CORE_TYPES_POWER_POLICY_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#include "core_types/app_id.h"

typedef enum {
    POWER_SOURCE_USB = 0,
    POWER_SOURCE_BATTERY,
} power_source_t;

typedef enum {
    DISPLAY_STATE_ACTIVE = 0,
    DISPLAY_STATE_DIM,
    DISPLAY_STATE_SLEEP,
} display_state_t;

typedef enum {
    REFRESH_MODE_REALTIME = 0,
    REFRESH_MODE_INTERACTIVE_POLL,
    REFRESH_MODE_BACKGROUND_CACHE,
    REFRESH_MODE_PAUSED,
} refresh_mode_t;

typedef struct {
    power_source_t power_source;
    display_state_t display_state;
    app_id_t foreground_app;
    bool wifi_connected;
    bool thermal_throttled;
    bool voltage_guard_triggered;
    bool user_interacting;
} power_policy_input_t;

typedef struct {
    uint8_t brightness_percent;
    refresh_mode_t claude_mode;
    refresh_mode_t market_mode;
    bool weather_refresh_allowed;
    bool slot_compute_allowed;
    bool slot_compute_throttled;
    bool should_enter_sleep;
} power_policy_output_t;

#endif
