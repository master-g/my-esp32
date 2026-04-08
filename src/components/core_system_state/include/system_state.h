#ifndef SYSTEM_STATE_H
#define SYSTEM_STATE_H

#include <stdbool.h>

#include "core_types/app_id.h"
#include "core_types/power_policy_types.h"
#include "esp_err.h"

esp_err_t system_state_init(void);
const power_policy_input_t *system_state_get_power_policy_input(void);
uint32_t system_state_get_user_activity_seq(void);
void system_state_set_power_source(power_source_t power_source);
void system_state_set_display_state(display_state_t display_state);
void system_state_set_foreground_app(app_id_t app_id);
void system_state_set_wifi_connected(bool wifi_connected);
void system_state_set_user_interacting(bool user_interacting);
void system_state_note_user_activity(void);

#endif
