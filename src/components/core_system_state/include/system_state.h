#ifndef SYSTEM_STATE_H
#define SYSTEM_STATE_H

#include <stdbool.h>

#include "core_types/app_id.h"
#include "core_types/power_policy_types.h"
#include "esp_err.h"

esp_err_t system_state_init(void);
void system_state_get_power_policy_input(power_policy_input_t *out);
void system_state_set_foreground_app(app_id_t app_id);
void system_state_set_wifi_connected(bool wifi_connected);

#endif
