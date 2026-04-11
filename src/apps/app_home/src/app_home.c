#include "app_home.h"

#include "home_runtime.h"

static home_runtime_t s_runtime;

static esp_err_t app_home_init(void) { return home_runtime_init(&s_runtime); }

static lv_obj_t *app_home_create_root(lv_obj_t *parent)
{
    return home_runtime_create_root(&s_runtime, parent);
}

static void app_home_resume(void) { home_runtime_resume(&s_runtime); }

static void app_home_suspend(void) { home_runtime_suspend(&s_runtime); }

static void app_home_handle_event(const app_event_t *event)
{
    home_runtime_handle_event(&s_runtime, event);
}

static esp_err_t app_home_handle_control(app_control_type_t type, const void *payload)
{
    return home_runtime_handle_control(&s_runtime, type, payload);
}

const app_descriptor_t *app_home_get_descriptor(void)
{
    static const app_descriptor_t descriptor = {
        .id = APP_ID_HOME,
        .name = "Home",
        .init = app_home_init,
        .create_root = app_home_create_root,
        .resume = app_home_resume,
        .suspend = app_home_suspend,
        .handle_event = app_home_handle_event,
        .handle_control = app_home_handle_control,
    };

    return &descriptor;
}
