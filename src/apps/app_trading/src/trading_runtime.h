#ifndef APP_TRADING_RUNTIME_H
#define APP_TRADING_RUNTIME_H

#include "app_manager.h"
#include "esp_err.h"
#include "trading_view.h"

typedef struct {
    trading_view_t view;
} trading_runtime_t;

esp_err_t trading_runtime_init(trading_runtime_t *runtime);
lv_obj_t *trading_runtime_create_root(trading_runtime_t *runtime, lv_obj_t *parent);
void trading_runtime_resume(trading_runtime_t *runtime);
void trading_runtime_suspend(trading_runtime_t *runtime);
void trading_runtime_handle_event(trading_runtime_t *runtime, const app_event_t *event);

#endif
