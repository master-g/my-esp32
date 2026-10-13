#include "power_policy.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static power_policy_input_t s_input;
static power_policy_output_t s_output;
static SemaphoreHandle_t s_mutex;

static bool same_input(const power_policy_input_t *a, const power_policy_input_t *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

static bool same_output(const power_policy_output_t *a, const power_policy_output_t *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

static power_policy_output_t compute_output(const power_policy_input_t *input)
{
    power_policy_output_t output;

    memset(&output, 0, sizeof(output));
    output.brightness_percent = 100;
    output.market_mode = (input->foreground_app == APP_ID_TRADING) ? REFRESH_MODE_REALTIME
                                                                   : REFRESH_MODE_BACKGROUND_CACHE;
    return output;
}

esp_err_t power_policy_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memset(&s_input, 0, sizeof(s_input));
    memset(&s_output, 0, sizeof(s_output));
    s_input.foreground_app = APP_ID_HOME;
    s_output = compute_output(&s_input);
    return ESP_OK;
}

bool power_policy_on_input_changed(const power_policy_input_t *input)
{
    power_policy_output_t next_output;
    bool changed = false;

    if (input == NULL) {
        return false;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    next_output = compute_output(input);
    changed = !same_input(&s_input, input) || !same_output(&s_output, &next_output);
    s_input = *input;
    s_output = next_output;
    xSemaphoreGive(s_mutex);
    return changed;
}

void power_policy_get_output(power_policy_output_t *out)
{
    if (out == NULL) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_output;
    xSemaphoreGive(s_mutex);
}
