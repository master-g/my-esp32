#include "system_state.h"

#include <string.h>

#include "core_types/app_event.h"
#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "power_policy.h"

static power_policy_input_t s_input;
static uint32_t s_user_activity_seq;
static SemaphoreHandle_t s_mutex;
static bool s_initialized;

static void recompute_and_publish_if_needed(void)
{
    power_policy_output_t output;
    app_event_t event = {
        .type = APP_EVENT_POWER_CHANGED,
        .payload = NULL,
    };

    if (!s_initialized) {
        return;
    }

    if (power_policy_on_input_changed(&s_input)) {
        power_policy_get_output(&output);
        event.payload = &output;
        event_bus_publish(&event);
    }
}

esp_err_t system_state_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memset(&s_input, 0, sizeof(s_input));
    s_input.power_source = POWER_SOURCE_USB;
    s_input.display_state = DISPLAY_STATE_ACTIVE;
    s_input.foreground_app = APP_ID_HOME;
    s_user_activity_seq = 0;
    s_initialized = true;
    recompute_and_publish_if_needed();
    return ESP_OK;
}

void system_state_get_power_policy_input(power_policy_input_t *out)
{
    if (out == NULL) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_input;
    xSemaphoreGive(s_mutex);
}

uint32_t system_state_get_user_activity_seq(void) { return s_user_activity_seq; }

void system_state_set_power_source(power_source_t power_source)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_input.power_source = power_source;
    recompute_and_publish_if_needed();
    xSemaphoreGive(s_mutex);
}

void system_state_set_display_state(display_state_t display_state)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_input.display_state = display_state;
    recompute_and_publish_if_needed();
    xSemaphoreGive(s_mutex);
}

void system_state_set_foreground_app(app_id_t app_id)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_input.foreground_app = app_id;
    recompute_and_publish_if_needed();
    xSemaphoreGive(s_mutex);
}

void system_state_set_wifi_connected(bool wifi_connected)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_input.wifi_connected = wifi_connected;
    recompute_and_publish_if_needed();
    xSemaphoreGive(s_mutex);
}

void system_state_set_user_interacting(bool user_interacting)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_input.user_interacting = user_interacting;
    recompute_and_publish_if_needed();
    xSemaphoreGive(s_mutex);
}

void system_state_note_user_activity(void) { s_user_activity_seq++; }
