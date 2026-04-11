#ifndef APP_HOME_HOME_RUNTIME_H
#define APP_HOME_HOME_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "app_manager.h"
#include "esp_err.h"
#include "home_approval.h"
#include "home_screensaver.h"
#include "home_view.h"

typedef struct {
    home_view_t view;
    home_screensaver_t screensaver;
    home_approval_t approval;
    lv_timer_t *time_refresh_timer;
    lv_timer_t *unread_timer;
    uint32_t unread_seq;
    bool was_connected;
} home_runtime_t;

esp_err_t home_runtime_init(home_runtime_t *runtime);
lv_obj_t *home_runtime_create_root(home_runtime_t *runtime, lv_obj_t *parent);
void home_runtime_resume(home_runtime_t *runtime);
void home_runtime_suspend(home_runtime_t *runtime);
void home_runtime_handle_event(home_runtime_t *runtime, const app_event_t *event);
esp_err_t home_runtime_handle_control(home_runtime_t *runtime, app_control_type_t type,
                                      const void *payload);

#endif
