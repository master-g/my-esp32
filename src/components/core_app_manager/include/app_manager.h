#ifndef APP_MANAGER_H
#define APP_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_types/app_event.h"
#include "core_types/app_id.h"
#include "core_types/lvgl_forward.h"
#include "esp_err.h"

typedef enum {
    APP_CONTROL_HOME_SCREENSAVER = 0,
    APP_CONTROL_CAPTURE_SCREENSHOT,
} app_control_type_t;

typedef struct {
    bool enabled;
} app_control_home_screensaver_t;

typedef enum {
    APP_SCREENSHOT_FORMAT_RGB565 = 0,
} app_screenshot_format_t;

typedef enum {
    APP_SCREENSHOT_SOURCE_LVGL = 0,
    APP_SCREENSHOT_SOURCE_HOME_DIRECT,
} app_screenshot_source_t;

typedef struct {
    app_id_t app_id;
    app_screenshot_format_t format;
    app_screenshot_source_t source;
    uint16_t width;
    uint16_t height;
    uint16_t stride_bytes;
} app_screenshot_info_t;

typedef struct {
    uint8_t *buffer;
    size_t capacity_bytes;
    size_t data_size;
    app_screenshot_info_t info;
} app_screenshot_t;

typedef struct {
    app_id_t id;
    const char *name;
    esp_err_t (*init)(void);
    lv_obj_t *(*create_root)(lv_obj_t *parent);
    void (*resume)(void);
    void (*suspend)(void);
    void (*handle_event)(const app_event_t *event);
    esp_err_t (*handle_control)(app_control_type_t type, const void *payload);
} app_descriptor_t;

#define APP_MANAGER_DEBUG_NAME_MAX 32

typedef struct {
    uint32_t ui_event_queue_drops;
    char last_dropped_event[APP_MANAGER_DEBUG_NAME_MAX];
} app_manager_debug_stats_t;

esp_err_t app_manager_init(void);
esp_err_t app_manager_register(const app_descriptor_t *descriptor);
esp_err_t app_manager_switch_to(app_id_t app_id);
esp_err_t app_manager_post_switch_to(app_id_t app_id);
esp_err_t app_manager_request_switch_to(app_id_t app_id, uint32_t timeout_ms);
esp_err_t app_manager_request_home_screensaver(bool enabled, uint32_t timeout_ms);
esp_err_t app_manager_request_screenshot(app_screenshot_t *capture, uint32_t timeout_ms);
app_id_t app_manager_get_foreground_app(void);
const app_descriptor_t *app_manager_get_descriptor(app_id_t app_id);
void app_manager_get_debug_stats(app_manager_debug_stats_t *out);
void app_manager_on_event(const app_event_t *event, void *context);
void app_manager_process_ui_events(void);

#endif
