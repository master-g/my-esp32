#include "power_runtime.h"

#include <stdint.h>

#include "bsp_board.h"
#include "core_types/app_event.h"
#include "esp_check.h"
#include "esp_log.h"
#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "power_policy.h"

static const char *TAG = "power_runtime";

#define POWER_RUNTIME_QUEUE_LEN 8
#define POWER_RUNTIME_TASK_STACK 4096
#define POWER_RUNTIME_TASK_PRIO 4

typedef enum {
    POWER_RUNTIME_CMD_APPLY_OUTPUT = 1,
} power_runtime_cmd_t;

static QueueHandle_t s_cmd_queue;
static TaskHandle_t s_task_handle;
static bool s_initialized;

static void apply_policy_output(void)
{
    power_policy_output_t output;

    power_policy_get_output(&output);
    bsp_board_set_backlight_percent(output.brightness_percent);
}

static void power_runtime_task(void *arg)
{
    power_runtime_cmd_t cmd;

    (void)arg;
    for (;;) {
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) == pdTRUE &&
            cmd == POWER_RUNTIME_CMD_APPLY_OUTPUT) {
            apply_policy_output();
        }
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

    ESP_RETURN_ON_ERROR(event_bus_subscribe(power_runtime_event_handler, NULL), TAG,
                        "failed to subscribe power runtime");

    {
        BaseType_t ret =
            xTaskCreatePinnedToCore(power_runtime_task, "power_runtime", POWER_RUNTIME_TASK_STACK,
                                    NULL, POWER_RUNTIME_TASK_PRIO, &s_task_handle, 1);
        ESP_RETURN_ON_FALSE(ret == pdPASS, ESP_ERR_NO_MEM, TAG, "power runtime task create failed");
    }

    s_initialized = true;
    (void)xQueueSend(s_cmd_queue, &cmd, 0);
    return ESP_OK;
}
