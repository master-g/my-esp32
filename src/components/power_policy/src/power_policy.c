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

    switch (input->display_state) {
    case DISPLAY_STATE_ACTIVE:
        output.brightness_percent = (input->power_source == POWER_SOURCE_USB) ? 100 : 60;
        break;
    case DISPLAY_STATE_DIM:
        output.brightness_percent = (input->power_source == POWER_SOURCE_USB) ? 35 : 20;
        break;
    case DISPLAY_STATE_SLEEP:
    default:
        output.brightness_percent = 0;
        break;
    }

    output.weather_refresh_allowed = (input->display_state != DISPLAY_STATE_SLEEP);
    output.should_enter_sleep = (input->display_state == DISPLAY_STATE_SLEEP);

    if (input->display_state == DISPLAY_STATE_SLEEP) {
        output.claude_mode = REFRESH_MODE_PAUSED;
        output.market_mode = REFRESH_MODE_PAUSED;
    } else if (input->display_state == DISPLAY_STATE_DIM) {
        output.claude_mode = REFRESH_MODE_BACKGROUND_CACHE;
        output.market_mode = REFRESH_MODE_PAUSED;
    } else if (input->power_source == POWER_SOURCE_USB) {
        output.claude_mode = (input->foreground_app == APP_ID_HOME) ? REFRESH_MODE_REALTIME
                                                                    : REFRESH_MODE_BACKGROUND_CACHE;
        output.market_mode = (input->foreground_app == APP_ID_TRADING)
                                 ? REFRESH_MODE_REALTIME
                                 : REFRESH_MODE_BACKGROUND_CACHE;
    } else {
        output.claude_mode = (input->foreground_app == APP_ID_HOME) ? REFRESH_MODE_INTERACTIVE_POLL
                                                                    : REFRESH_MODE_BACKGROUND_CACHE;
        output.market_mode = (input->foreground_app == APP_ID_TRADING)
                                 ? REFRESH_MODE_INTERACTIVE_POLL
                                 : REFRESH_MODE_PAUSED;
    }

    output.slot_compute_allowed = (input->display_state == DISPLAY_STATE_ACTIVE) &&
                                  (input->foreground_app == APP_ID_SATOSHI_SLOT) &&
                                  !input->thermal_throttled && !input->voltage_guard_triggered;
    output.slot_compute_throttled =
        output.slot_compute_allowed && (input->power_source == POWER_SOURCE_BATTERY);

    if (input->thermal_throttled || input->voltage_guard_triggered) {
        output.slot_compute_allowed = false;
        output.slot_compute_throttled = false;
        if (output.market_mode == REFRESH_MODE_REALTIME) {
            output.market_mode = REFRESH_MODE_INTERACTIVE_POLL;
        }
    }

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
    s_input.power_source = POWER_SOURCE_USB;
    s_input.display_state = DISPLAY_STATE_ACTIVE;
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

void power_policy_get_input(power_policy_input_t *out)
{
    if (out == NULL) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_input;
    xSemaphoreGive(s_mutex);
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

bool power_policy_is_refresh_mode(refresh_mode_t expected, app_id_t app_id)
{
    bool result = false;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    switch (app_id) {
    case APP_ID_HOME:
        result = (s_output.claude_mode == expected);
        break;
    case APP_ID_TRADING:
        result = (s_output.market_mode == expected);
        break;
    default:
        break;
    }
    xSemaphoreGive(s_mutex);
    return result;
}
