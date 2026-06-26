#ifndef SYSTEM_STATE_H
#define SYSTEM_STATE_H

#include <stdbool.h>

#include "core_types/app_id.h"
#include "core_types/power_policy_types.h"
#include "esp_err.h"

esp_err_t system_state_init(void);
void system_state_get_power_policy_input(power_policy_input_t *out);
uint32_t system_state_get_user_activity_seq(void);
void system_state_set_power_source(power_source_t power_source);
void system_state_set_display_state(display_state_t display_state);
void system_state_set_foreground_app(app_id_t app_id);
void system_state_set_wifi_connected(bool wifi_connected);
void system_state_set_user_interacting(bool user_interacting);
void system_state_note_user_activity(void);

/* Waiting-alert flag. Set by the home view while a request is pending, read by
 * power_runtime to keep the screen lit (steady, not dimmed) while it is set.
 * Deliberately OUTSIDE the power-policy input and the recompute/publish path —
 * toggling it must not emit APP_EVENT_POWER_CHANGED (that would force a backlight
 * rewrite each show/hide). */
void system_state_set_alert_active(bool alert_active);
bool system_state_get_alert_active(void);

#endif
