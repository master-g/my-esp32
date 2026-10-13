#ifndef POWER_POLICY_H
#define POWER_POLICY_H

#include <stdbool.h>

#include "core_types/power_policy_types.h"
#include "esp_err.h"

esp_err_t power_policy_init(void);
bool power_policy_on_input_changed(const power_policy_input_t *input);
void power_policy_get_output(power_policy_output_t *out);

#endif
