#include "app_trading.h"

#include "trading_runtime.h"

static trading_runtime_t s_runtime;

static esp_err_t app_trading_init(void) { return trading_runtime_init(&s_runtime); }

static lv_obj_t *app_trading_create_root(lv_obj_t *parent)
{
    return trading_runtime_create_root(&s_runtime, parent);
}

static void app_trading_resume(void) { trading_runtime_resume(&s_runtime); }

static void app_trading_suspend(void) { trading_runtime_suspend(&s_runtime); }

static void app_trading_handle_event(const app_event_t *event)
{
    trading_runtime_handle_event(&s_runtime, event);
}

const app_descriptor_t *app_trading_get_descriptor(void)
{
    static const app_descriptor_t descriptor = {
        .id = APP_ID_TRADING,
        .name = "Trading",
        .init = app_trading_init,
        .create_root = app_trading_create_root,
        .resume = app_trading_resume,
        .suspend = app_trading_suspend,
        .handle_event = app_trading_handle_event,
    };

    return &descriptor;
}
