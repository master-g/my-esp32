#include "app_manager.h"

#include <stdbool.h>
#include <stddef.h>

#include "bsp_board.h"
#include "esp_check.h"
#include "esp_log.h"
#include "lvgl.h"

#define APP_MANAGER_MAX_APPS APP_ID_COUNT

typedef struct {
    app_descriptor_t descriptor;
    bool in_use;
    bool root_created;
    lv_obj_t *root;
} app_slot_t;

static const char *TAG = "app_manager";
static app_slot_t s_slots[APP_MANAGER_MAX_APPS];
static app_id_t s_foreground_app = APP_ID_INVALID;
static bool s_initialized;

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
        if (!bsp_board_lock(UINT32_MAX)) {
            return;
        }
        slot->descriptor.handle_event(event);
        bsp_board_unlock();
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
        return ESP_OK;
    }

    current_slot = find_slot_mut(s_foreground_app);
    if (current_slot != NULL && current_slot->descriptor.suspend != NULL) {
        current_slot->descriptor.suspend();
    }

    s_foreground_app = app_id;

    if (!bsp_board_lock(UINT32_MAX)) {
        return ESP_ERR_TIMEOUT;
    }

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
    bsp_board_unlock();

    if (next_slot->descriptor.resume != NULL) {
        next_slot->descriptor.resume();
    }
    dispatch_to_app(next_slot, &enter_event);
    dispatch_to_app(current_slot, &leave_event);

    ESP_LOGI(TAG, "Foreground app -> %s", app_id_to_string(app_id));
    return ESP_OK;
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
    const app_slot_t *slot = NULL;

    (void)context;
    if (!s_initialized || event == NULL) {
        return;
    }

    if (event->type == APP_EVENT_ENTER || event->type == APP_EVENT_LEAVE) {
        return;
    }

    slot = find_slot(s_foreground_app);
    dispatch_to_app(slot, event);
}
