#ifndef EVENT_BUS_H
#define EVENT_BUS_H

#include "core_types/app_event.h"
#include "esp_err.h"

typedef void (*event_bus_handler_t)(const app_event_t *event, void *context);

esp_err_t event_bus_init(void);
esp_err_t event_bus_subscribe(event_bus_handler_t handler, void *context);
esp_err_t event_bus_publish(const app_event_t *event);

#endif
