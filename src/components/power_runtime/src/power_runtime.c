#include "power_runtime.h"

#include <stdint.h>

#include "bsp_board.h"
#include "core_types/app_event.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "power_policy.h"
#include "system_state.h"

static const char *TAG = "power_runtime";

#define POWER_RUNTIME_QUEUE_LEN 8
#define POWER_RUNTIME_TASK_STACK 4096
#define POWER_RUNTIME_TASK_PRIO 4
#define POWER_RUNTIME_POLL_MS 200
#define POWER_RUNTIME_USB_DIM_TIMEOUT_US (30LL * 1000 * 1000)
#define POWER_RUNTIME_USB_SLEEP_TIMEOUT_US (90LL * 1000 * 1000)
#define POWER_RUNTIME_BATTERY_DIM_TIMEOUT_US (15LL * 1000 * 1000)
#define POWER_RUNTIME_BATTERY_SLEEP_TIMEOUT_US (45LL * 1000 * 1000)

typedef enum {
    POWER_RUNTIME_CMD_APPLY_OUTPUT = 1,
} power_runtime_cmd_t;

static QueueHandle_t s_cmd_queue;
static TaskHandle_t s_task_handle;
static uint32_t s_seen_activity_seq;
static int64_t s_last_activity_us;
static bool s_initialized;

static int64_t get_dim_timeout_us(power_source_t source)
{
    return (source == POWER_SOURCE_BATTERY) ? POWER_RUNTIME_BATTERY_DIM_TIMEOUT_US
                                            : POWER_RUNTIME_USB_DIM_TIMEOUT_US;
}

static int64_t get_sleep_timeout_us(power_source_t source)
{
    return (source == POWER_SOURCE_BATTERY) ? POWER_RUNTIME_BATTERY_SLEEP_TIMEOUT_US
                                            : POWER_RUNTIME_USB_SLEEP_TIMEOUT_US;
}

static void apply_policy_output(void)
{
    power_policy_output_t output;

    power_policy_get_output(&output);
    bsp_board_set_backlight_percent(output.brightness_percent);
}

static void maybe_update_display_state_from_idle(void)
{
    power_policy_input_t input;
    int64_t idle_us;

    system_state_get_power_policy_input(&input);
    idle_us = esp_timer_get_time() - s_last_activity_us;

    if (input.power_source == POWER_SOURCE_USB) {
        if (input.display_state != DISPLAY_STATE_ACTIVE) {
            ESP_LOGI(TAG, "USB power -> force ACTIVE");
            system_state_set_display_state(DISPLAY_STATE_ACTIVE);
        }
        return;
    }

    if (input.display_state == DISPLAY_STATE_ACTIVE &&
        idle_us >= get_dim_timeout_us(input.power_source)) {
        ESP_LOGI(TAG, "Idle timeout -> DIM");
        system_state_set_display_state(DISPLAY_STATE_DIM);
    } else if (input.display_state == DISPLAY_STATE_DIM &&
               idle_us >= get_sleep_timeout_us(input.power_source)) {
        ESP_LOGI(TAG, "Idle timeout -> SLEEP");
        system_state_set_display_state(DISPLAY_STATE_SLEEP);
    }
}

static void reconcile_user_activity(void)
{
    power_policy_input_t input;
    const uint32_t activity_seq = system_state_get_user_activity_seq();

    if (activity_seq == s_seen_activity_seq) {
        return;
    }

    s_seen_activity_seq = activity_seq;
    s_last_activity_us = esp_timer_get_time();

    system_state_get_power_policy_input(&input);
    if (input.display_state != DISPLAY_STATE_ACTIVE) {
        ESP_LOGI(TAG, "User activity -> ACTIVE");
        system_state_set_display_state(DISPLAY_STATE_ACTIVE);
    }
}

static void power_runtime_task(void *arg)
{
    power_runtime_cmd_t cmd;

    (void)arg;
    for (;;) {
        if (xQueueReceive(s_cmd_queue, &cmd, pdMS_TO_TICKS(POWER_RUNTIME_POLL_MS)) == pdTRUE) {
            if (cmd == POWER_RUNTIME_CMD_APPLY_OUTPUT) {
                apply_policy_output();
            }
        }

        reconcile_user_activity();
        maybe_update_display_state_from_idle();
    }
}

static void power_runtime_event_handler(const app_event_t *event, void *context)
{
    power_runtime_cmd_t cmd = POWER_RUNTIME_CMD_APPLY_OUTPUT;

    (void)context;
    if (event == NULL || !s_initialized || s_cmd_queue == NULL) {
        return;
    }

    if (event->type != APP_EVENT_POWER_CHANGED) {
        return;
    }

    (void)xQueueSend(s_cmd_queue, &cmd, 0);
}

esp_err_t power_runtime_init(void)
{
    power_runtime_cmd_t cmd = POWER_RUNTIME_CMD_APPLY_OUTPUT;

    if (s_initialized) {
        return ESP_OK;
    }

    s_cmd_queue = xQueueCreate(POWER_RUNTIME_QUEUE_LEN, sizeof(power_runtime_cmd_t));
    ESP_RETURN_ON_FALSE(s_cmd_queue != NULL, ESP_ERR_NO_MEM, TAG,
                        "failed to create power runtime queue");

    s_seen_activity_seq = system_state_get_user_activity_seq();
    s_last_activity_us = esp_timer_get_time();

    ESP_RETURN_ON_ERROR(event_bus_subscribe(power_runtime_event_handler, NULL), TAG,
                        "failed to subscribe power runtime");

    {
        BaseType_t ret = xTaskCreatePinnedToCore(power_runtime_task, "power_runtime",
                                                  POWER_RUNTIME_TASK_STACK, NULL,
                                                  POWER_RUNTIME_TASK_PRIO, &s_task_handle, 1);
        ESP_RETURN_ON_FALSE(ret == pdPASS, ESP_ERR_NO_MEM, TAG, "power runtime task create failed");
    }

    s_initialized = true;
    (void)xQueueSend(s_cmd_queue, &cmd, 0);
    return ESP_OK;
}
