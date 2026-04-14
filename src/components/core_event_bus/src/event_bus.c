#include "event_bus.h"

#include <stddef.h>

#include "esp_check.h"

/* Keep a little slack above the current subscriber set so new services do not fail at boot. */
#define EVENT_BUS_MAX_SUBSCRIBERS 12

typedef struct {
    event_bus_handler_t handler;
    void *context;
} event_bus_subscription_t;

static event_bus_subscription_t s_subscriptions[EVENT_BUS_MAX_SUBSCRIBERS];
static size_t s_subscription_count;
static bool s_initialized;

esp_err_t event_bus_init(void)
{
    size_t i = 0;

    for (i = 0; i < EVENT_BUS_MAX_SUBSCRIBERS; ++i) {
        s_subscriptions[i].handler = NULL;
        s_subscriptions[i].context = NULL;
    }

    s_subscription_count = 0;
    s_initialized = true;
    return ESP_OK;
}

esp_err_t event_bus_subscribe(event_bus_handler_t handler, void *context)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, "event_bus",
                        "event bus not initialized");
    ESP_RETURN_ON_FALSE(handler != NULL, ESP_ERR_INVALID_ARG, "event_bus", "handler is required");
    ESP_RETURN_ON_FALSE(s_subscription_count < EVENT_BUS_MAX_SUBSCRIBERS, ESP_ERR_NO_MEM,
                        "event_bus", "subscription table full");

    s_subscriptions[s_subscription_count].handler = handler;
    s_subscriptions[s_subscription_count].context = context;
    s_subscription_count++;

    return ESP_OK;
}

esp_err_t event_bus_publish(const app_event_t *event)
{
    size_t i = 0;

    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, "event_bus",
                        "event bus not initialized");
    ESP_RETURN_ON_FALSE(event != NULL, ESP_ERR_INVALID_ARG, "event_bus", "event is required");

    for (i = 0; i < s_subscription_count; ++i) {
        if (s_subscriptions[i].handler != NULL) {
            s_subscriptions[i].handler(event, s_subscriptions[i].context);
        }
    }

    return ESP_OK;
}
