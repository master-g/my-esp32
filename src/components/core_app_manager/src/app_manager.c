#include "app_manager.h"

#include <stdbool.h>
#include <stddef.h>

#include "bsp_board.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "system_state.h"

#define APP_MANAGER_MAX_APPS APP_ID_COUNT
#define UI_EVENT_QUEUE_LEN 16
#define UI_CONTROL_QUEUE_LEN 4
#define UI_CONTROL_NOTIFY_INDEX 0

typedef struct {
    app_descriptor_t descriptor;
    bool in_use;
    bool root_created;
    lv_obj_t *root;
} app_slot_t;

typedef enum {
    UI_CONTROL_SWITCH_TO = 0,
    UI_CONTROL_HOME_SCREENSAVER,
} ui_control_type_t;

typedef struct {
    ui_control_type_t type;
    TaskHandle_t waiter_task;
    uint16_t request_id;
    union {
        app_id_t app_id;
        bool screensaver_enabled;
    } arg;
} ui_control_request_t;

static const char *TAG = "app_manager";
static app_slot_t s_slots[APP_MANAGER_MAX_APPS];
static app_id_t s_foreground_app = APP_ID_INVALID;
static bool s_initialized;
static QueueHandle_t s_ui_event_queue;
static QueueHandle_t s_ui_control_queue;
static uint16_t s_ui_control_request_seq;
static portMUX_TYPE s_ui_control_request_lock = portMUX_INITIALIZER_UNLOCKED;

static app_slot_t *find_slot_mut(app_id_t app_id);
static const app_slot_t *find_slot(app_id_t app_id);
static void dispatch_to_app(const app_slot_t *slot, const app_event_t *event);
static TickType_t timeout_to_ticks(uint32_t timeout_ms);
static esp_err_t request_ui_control(const ui_control_request_t *request, uint32_t timeout_ms);
static esp_err_t execute_ui_control(const ui_control_request_t *request);
static uint16_t next_ui_control_request_id(void);
static uint32_t pack_ui_control_result(uint16_t request_id, esp_err_t err);
static uint16_t unpack_ui_control_request_id(uint32_t notification_value);
static esp_err_t unpack_ui_control_result(uint32_t notification_value);

static esp_err_t switch_to_locked(app_id_t app_id)
{
    app_slot_t *next_slot = NULL;
    app_slot_t *current_slot = NULL;
    app_event_t enter_event = {
        .type = APP_EVENT_ENTER,
        .payload = NULL,
    };
    app_event_t leave_event = {
        .type = APP_EVENT_LEAVE,
        .payload = NULL,
    };

    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "app manager not initialized");
    next_slot = find_slot_mut(app_id);
    ESP_RETURN_ON_FALSE(next_slot != NULL, ESP_ERR_NOT_FOUND, TAG, "target app is not registered");

    if (s_foreground_app == app_id) {
        system_state_set_foreground_app(app_id);
        return ESP_OK;
    }

    current_slot = find_slot_mut(s_foreground_app);
    if (current_slot != NULL && current_slot->descriptor.suspend != NULL) {
        current_slot->descriptor.suspend();
    }

    s_foreground_app = app_id;
    system_state_set_foreground_app(app_id);

    if (current_slot != NULL && current_slot->root != NULL) {
        lv_obj_add_flag(current_slot->root, LV_OBJ_FLAG_HIDDEN);
    }

    if (!next_slot->root_created && next_slot->descriptor.create_root != NULL) {
        next_slot->root = next_slot->descriptor.create_root(bsp_board_get_app_root());
        if (next_slot->root != NULL) {
            lv_obj_add_flag(next_slot->root, LV_OBJ_FLAG_HIDDEN);
        }
        next_slot->root_created = true;
    }

    if (next_slot->root != NULL) {
        lv_obj_clear_flag(next_slot->root, LV_OBJ_FLAG_HIDDEN);
    }

    if (next_slot->descriptor.resume != NULL) {
        next_slot->descriptor.resume();
    }
    dispatch_to_app(next_slot, &enter_event);
    dispatch_to_app(current_slot, &leave_event);

    ESP_LOGI(TAG, "Foreground app -> %s", app_id_to_string(app_id));
    return ESP_OK;
}

static TickType_t timeout_to_ticks(uint32_t timeout_ms)
{
    return (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
}

static uint16_t next_ui_control_request_id(void)
{
    uint16_t next_id;

    taskENTER_CRITICAL(&s_ui_control_request_lock);
    s_ui_control_request_seq++;
    if (s_ui_control_request_seq == 0) {
        s_ui_control_request_seq = 1;
    }
    next_id = s_ui_control_request_seq;
    taskEXIT_CRITICAL(&s_ui_control_request_lock);

    return next_id;
}

static uint32_t pack_ui_control_result(uint16_t request_id, esp_err_t err)
{
    return ((uint32_t)request_id << 16) | ((uint32_t)err & 0xFFFFU);
}

static uint16_t unpack_ui_control_request_id(uint32_t notification_value)
{
    return (uint16_t)(notification_value >> 16);
}

static esp_err_t unpack_ui_control_result(uint32_t notification_value)
{
    return (esp_err_t)(notification_value & 0xFFFFU);
}

static esp_err_t request_ui_control(const ui_control_request_t *request, uint32_t timeout_ms)
{
    TickType_t timeout_ticks;
    uint32_t notification_value = 0;
    ui_control_request_t queued_request;
    TickType_t start_ticks;
    TickType_t elapsed_ticks;
    TickType_t remaining_ticks;

    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "app manager not initialized");
    ESP_RETURN_ON_FALSE(s_ui_control_queue != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "ui control queue missing");
    ESP_RETURN_ON_FALSE(request != NULL, ESP_ERR_INVALID_ARG, TAG, "request is required");

    timeout_ticks = timeout_to_ticks(timeout_ms);
    queued_request = *request;
    queued_request.waiter_task = xTaskGetCurrentTaskHandle();
    queued_request.request_id = next_ui_control_request_id();

    (void)xTaskNotifyWaitIndexed(UI_CONTROL_NOTIFY_INDEX, 0, UINT32_MAX, &notification_value, 0);

    if (xQueueSend(s_ui_control_queue, &queued_request, timeout_ticks) != pdTRUE) {
        ESP_LOGW(TAG, "ui control queue full (type=%d)", (int)queued_request.type);
        return ESP_ERR_TIMEOUT;
    }

    start_ticks = xTaskGetTickCount();
    remaining_ticks = timeout_ticks;

    for (;;) {
        if (xTaskNotifyWaitIndexed(UI_CONTROL_NOTIFY_INDEX, 0, UINT32_MAX, &notification_value,
                                   remaining_ticks) != pdTRUE) {
            ESP_LOGW(TAG, "ui control timed out (type=%d)", (int)queued_request.type);
            return ESP_ERR_TIMEOUT;
        }
        if (unpack_ui_control_request_id(notification_value) == queued_request.request_id) {
            return unpack_ui_control_result(notification_value);
        }

        if (timeout_ticks == portMAX_DELAY) {
            continue;
        }

        elapsed_ticks = xTaskGetTickCount() - start_ticks;
        if (elapsed_ticks >= timeout_ticks) {
            ESP_LOGW(TAG, "ui control timed out after stale notify (type=%d)",
                     (int)queued_request.type);
            return ESP_ERR_TIMEOUT;
        }
        remaining_ticks = timeout_ticks - elapsed_ticks;
    }
}

static esp_err_t execute_ui_control(const ui_control_request_t *request)
{
    const app_slot_t *slot = NULL;

    if (request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (request->type) {
    case UI_CONTROL_SWITCH_TO:
        return switch_to_locked(request->arg.app_id);
    case UI_CONTROL_HOME_SCREENSAVER: {
        app_control_home_screensaver_t control = {
            .enabled = request->arg.screensaver_enabled,
        };

        ESP_RETURN_ON_ERROR(switch_to_locked(APP_ID_HOME), TAG, "home switch failed");
        slot = find_slot(APP_ID_HOME);
        ESP_RETURN_ON_FALSE(slot != NULL, ESP_ERR_NOT_FOUND, TAG, "home app not registered");
        ESP_RETURN_ON_FALSE(slot->descriptor.handle_control != NULL, ESP_ERR_NOT_SUPPORTED, TAG,
                            "home app does not support controls");
        return slot->descriptor.handle_control(APP_CONTROL_HOME_SCREENSAVER, &control);
    }
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

static app_slot_t *find_slot_mut(app_id_t app_id)
{
    size_t i = 0;

    for (i = 0; i < APP_MANAGER_MAX_APPS; ++i) {
        if (s_slots[i].in_use && s_slots[i].descriptor.id == app_id) {
            return &s_slots[i];
        }
    }

    return NULL;
}

static const app_slot_t *find_slot(app_id_t app_id) { return find_slot_mut(app_id); }

static void dispatch_to_app(const app_slot_t *slot, const app_event_t *event)
{
    if (slot != NULL && slot->descriptor.handle_event != NULL) {
        slot->descriptor.handle_event(event);
    }
}

esp_err_t app_manager_init(void)
{
    size_t i = 0;

    for (i = 0; i < APP_MANAGER_MAX_APPS; ++i) {
        s_slots[i].in_use = false;
        s_slots[i].root_created = false;
        s_slots[i].descriptor.id = APP_ID_INVALID;
        s_slots[i].descriptor.name = NULL;
        s_slots[i].descriptor.init = NULL;
        s_slots[i].descriptor.create_root = NULL;
        s_slots[i].descriptor.resume = NULL;
        s_slots[i].descriptor.suspend = NULL;
        s_slots[i].descriptor.handle_event = NULL;
        s_slots[i].root = NULL;
    }

    s_foreground_app = APP_ID_INVALID;
    s_ui_event_queue = xQueueCreate(UI_EVENT_QUEUE_LEN, sizeof(app_event_type_t));
    s_ui_control_queue = xQueueCreate(UI_CONTROL_QUEUE_LEN, sizeof(ui_control_request_t));
    s_ui_control_request_seq = 0;
    ESP_RETURN_ON_FALSE(s_ui_event_queue != NULL, ESP_ERR_NO_MEM, TAG,
                        "ui event queue alloc failed");
    ESP_RETURN_ON_FALSE(s_ui_control_queue != NULL, ESP_ERR_NO_MEM, TAG,
                        "ui control queue alloc failed");
    s_initialized = true;
    return ESP_OK;
}

esp_err_t app_manager_register(const app_descriptor_t *descriptor)
{
    size_t i = 0;

    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "app manager not initialized");
    ESP_RETURN_ON_FALSE(descriptor != NULL, ESP_ERR_INVALID_ARG, TAG, "descriptor is required");
    ESP_RETURN_ON_FALSE(descriptor->id < APP_ID_COUNT, ESP_ERR_INVALID_ARG, TAG, "invalid app id");
    ESP_RETURN_ON_FALSE(find_slot(descriptor->id) == NULL, ESP_ERR_INVALID_STATE, TAG,
                        "app already registered");

    for (i = 0; i < APP_MANAGER_MAX_APPS; ++i) {
        if (!s_slots[i].in_use) {
            s_slots[i].descriptor = *descriptor;
            s_slots[i].in_use = true;
            if (s_slots[i].descriptor.init != NULL) {
                ESP_RETURN_ON_ERROR(s_slots[i].descriptor.init(), TAG, "app init failed");
            }
            return ESP_OK;
        }
    }

    return ESP_ERR_NO_MEM;
}

esp_err_t app_manager_switch_to(app_id_t app_id)
{
    if (!bsp_board_lock(UINT32_MAX)) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = switch_to_locked(app_id);
    bsp_board_unlock();
    return err;
}

esp_err_t app_manager_request_switch_to(app_id_t app_id, uint32_t timeout_ms)
{
    ui_control_request_t request = {
        .type = UI_CONTROL_SWITCH_TO,
        .waiter_task = NULL,
        .arg.app_id = app_id,
    };

    return request_ui_control(&request, timeout_ms);
}

esp_err_t app_manager_request_home_screensaver(bool enabled, uint32_t timeout_ms)
{
    ui_control_request_t request = {
        .type = UI_CONTROL_HOME_SCREENSAVER,
        .waiter_task = NULL,
        .arg.screensaver_enabled = enabled,
    };

    return request_ui_control(&request, timeout_ms);
}

app_id_t app_manager_get_foreground_app(void) { return s_foreground_app; }

const app_descriptor_t *app_manager_get_descriptor(app_id_t app_id)
{
    const app_slot_t *slot = find_slot(app_id);

    if (slot == NULL) {
        return NULL;
    }

    return &slot->descriptor;
}

void app_manager_on_event(const app_event_t *event, void *context)
{
    app_event_type_t event_type;

    (void)context;
    if (!s_initialized || event == NULL) {
        return;
    }

    if (event->type == APP_EVENT_ENTER || event->type == APP_EVENT_LEAVE) {
        return;
    }

    event_type = event->type;
    if (xQueueSend(s_ui_event_queue, &event_type, 0) != pdTRUE) {
        ESP_LOGW(TAG, "ui event queue full, dropping %s", app_event_type_to_string(event_type));
    }
}

void app_manager_process_ui_events(void)
{
    app_event_type_t event_type;
    const app_slot_t *slot = NULL;
    ui_control_request_t control_request;

    if (!s_initialized) {
        return;
    }

    while (xQueueReceive(s_ui_control_queue, &control_request, 0) == pdTRUE) {
        esp_err_t err = execute_ui_control(&control_request);
        if (control_request.waiter_task != NULL) {
            (void)xTaskNotifyIndexed(control_request.waiter_task, UI_CONTROL_NOTIFY_INDEX,
                                     pack_ui_control_result(control_request.request_id, err),
                                     eSetValueWithOverwrite);
        }
    }

    while (xQueueReceive(s_ui_event_queue, &event_type, 0) == pdTRUE) {
        slot = find_slot(s_foreground_app);
        if (slot != NULL) {
            app_event_t event = {
                .type = event_type,
                .payload = NULL,
            };

            dispatch_to_app(slot, &event);
        }
    }
}
