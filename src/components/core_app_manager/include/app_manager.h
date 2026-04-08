#ifndef APP_MANAGER_H
#define APP_MANAGER_H

#include "core_types/app_event.h"
#include "core_types/app_id.h"
#include "core_types/lvgl_forward.h"
#include "esp_err.h"

typedef struct {
    app_id_t id;
    const char *name;
    esp_err_t (*init)(void);
    lv_obj_t *(*create_root)(lv_obj_t *parent);
    void (*resume)(void);
    void (*suspend)(void);
    void (*handle_event)(const app_event_t *event);
} app_descriptor_t;

esp_err_t app_manager_init(void);
esp_err_t app_manager_register(const app_descriptor_t *descriptor);
esp_err_t app_manager_switch_to(app_id_t app_id);
app_id_t app_manager_get_foreground_app(void);
const app_descriptor_t *app_manager_get_descriptor(app_id_t app_id);
void app_manager_on_event(const app_event_t *event, void *context);

#endif
