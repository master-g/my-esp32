#include "service_settings.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "core_types/app_event.h"
#include "esp_check.h"
#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define SETTINGS_QUEUE_LEN 4
#define SETTINGS_TASK_STACK_SIZE (6 * 1024)

typedef enum {
    SETTINGS_CMD_SCAN = 0,
    SETTINGS_CMD_ADD_OR_UPDATE,
    SETTINGS_CMD_REMOVE,
} settings_command_type_t;

typedef struct {
    settings_command_type_t type;
    char ssid[NET_MANAGER_SSID_MAX];
    char password[NET_MANAGER_PASSWORD_MAX];
    bool password_provided;
    bool hidden;
} settings_command_t;

static const char *TAG = "settings_service";
static settings_snapshot_t s_snapshot;
static SemaphoreHandle_t s_mutex;
static QueueHandle_t s_queue;
static bool s_initialized;

static void publish_settings_event(void)
{
    app_event_t event = {
        .type = APP_EVENT_DATA_SETTINGS,
        .payload = NULL,
    };

    event_bus_publish(&event);
}

static void copy_text(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0) {
        return;
    }

    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    snprintf(dst, dst_size, "%s", src);
}

static void clear_command(settings_command_t *command)
{
    if (command == NULL) {
        return;
    }

    memset(command, 0, sizeof(*command));
}

static void refresh_snapshot_locked(void)
{
    net_manager_get_snapshot(&s_snapshot.net);
    net_manager_list_profiles(s_snapshot.profiles, NET_PROFILE_MAX, &s_snapshot.profile_count);
}

static void set_busy_locked(settings_service_operation_t operation, const char *message)
{
    s_snapshot.busy = true;
    s_snapshot.last_op_failed = false;
    s_snapshot.operation = operation;
    copy_text(s_snapshot.status_text, sizeof(s_snapshot.status_text), message);
}

static void finish_operation_locked(bool failed, const char *message)
{
    s_snapshot.busy = false;
    s_snapshot.last_op_failed = failed;
    s_snapshot.operation = SETTINGS_SERVICE_OP_IDLE;
    copy_text(s_snapshot.status_text, sizeof(s_snapshot.status_text), message);
}

static void handle_net_event(void)
{
    if (!s_initialized || s_mutex == NULL) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    refresh_snapshot_locked();
    xSemaphoreGive(s_mutex);
    publish_settings_event();
}

static void settings_service_event_handler(const app_event_t *event, void *context)
{
    (void)context;
    if (event == NULL) {
        return;
    }

    if (event->type == APP_EVENT_NET_CHANGED) {
        handle_net_event();
    }
}

static void settings_service_task(void *arg)
{
    settings_command_t *command = NULL;

    (void)arg;
    for (;;) {
        if (xQueueReceive(s_queue, &command, portMAX_DELAY) != pdTRUE || command == NULL) {
            continue;
        }

        switch (command->type) {
        case SETTINGS_CMD_SCAN: {
            net_scan_ap_t results[SETTINGS_SERVICE_SCAN_MAX] = {0};
            size_t count = 0;
            esp_err_t err =
                net_manager_scan_access_points(results, SETTINGS_SERVICE_SCAN_MAX, &count);

            xSemaphoreTake(s_mutex, portMAX_DELAY);
            refresh_snapshot_locked();
            memset(s_snapshot.scan_results, 0, sizeof(s_snapshot.scan_results));
            s_snapshot.scan_count = 0;
            if (err == ESP_OK) {
                memcpy(s_snapshot.scan_results, results, sizeof(results));
                s_snapshot.scan_count = count;
                finish_operation_locked(false,
                                        (count > 0) ? "Scan complete" : "No visible networks");
            } else {
                finish_operation_locked(true, "Wi-Fi scan failed");
            }
            xSemaphoreGive(s_mutex);
            publish_settings_event();
            break;
        }
        case SETTINGS_CMD_ADD_OR_UPDATE: {
            esp_err_t err = net_manager_add_or_update_profile(
                command->ssid, command->password_provided ? command->password : NULL,
                command->hidden);

            xSemaphoreTake(s_mutex, portMAX_DELAY);
            refresh_snapshot_locked();
            if (err == ESP_OK) {
                finish_operation_locked(false, "Profile saved");
            } else {
                finish_operation_locked(true, "Failed to save profile");
            }
            xSemaphoreGive(s_mutex);
            publish_settings_event();
            break;
        }
        case SETTINGS_CMD_REMOVE: {
            esp_err_t err = net_manager_remove_profile(command->ssid);

            xSemaphoreTake(s_mutex, portMAX_DELAY);
            refresh_snapshot_locked();
            if (err == ESP_OK) {
                finish_operation_locked(false, "Profile removed");
            } else {
                finish_operation_locked(true, "Failed to remove profile");
            }
            xSemaphoreGive(s_mutex);
            publish_settings_event();
            break;
        }
        default:
            break;
        }

        clear_command(command);
        free(command);
        command = NULL;
    }
}

esp_err_t settings_service_init(void)
{
    BaseType_t ret;

    if (s_initialized) {
        return ESP_OK;
    }

    s_mutex = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(SETTINGS_QUEUE_LEN, sizeof(settings_command_t *));
    ESP_RETURN_ON_FALSE(s_mutex != NULL && s_queue != NULL, ESP_ERR_NO_MEM, TAG,
                        "settings service alloc failed");

    memset(&s_snapshot, 0, sizeof(s_snapshot));
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    refresh_snapshot_locked();
    finish_operation_locked(false, "Ready");
    xSemaphoreGive(s_mutex);

    ESP_RETURN_ON_ERROR(event_bus_subscribe(settings_service_event_handler, NULL), TAG,
                        "failed to subscribe settings service");
    ret = xTaskCreatePinnedToCore(settings_service_task, "settings_service",
                                  SETTINGS_TASK_STACK_SIZE, NULL, 2, NULL, 1);
    ESP_RETURN_ON_FALSE(ret == pdPASS, ESP_ERR_NO_MEM, TAG, "settings task create failed");
    s_initialized = true;
    return ESP_OK;
}

void settings_service_get_snapshot(settings_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }

    if (!s_initialized || s_mutex == NULL) {
        memset(out, 0, sizeof(*out));
        copy_text(out->status_text, sizeof(out->status_text), "Settings unavailable");
        out->last_op_failed = true;
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_snapshot;
    xSemaphoreGive(s_mutex);
}

static esp_err_t enqueue_command(const settings_command_t *command)
{
    settings_command_t *queued_command = NULL;

    if (command == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized || s_mutex == NULL || s_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    queued_command = calloc(1, sizeof(*queued_command));
    ESP_RETURN_ON_FALSE(queued_command != NULL, ESP_ERR_NO_MEM, TAG,
                        "settings command alloc failed");
    *queued_command = *command;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_snapshot.busy) {
        xSemaphoreGive(s_mutex);
        clear_command(queued_command);
        free(queued_command);
        return ESP_ERR_INVALID_STATE;
    }

    switch (command->type) {
    case SETTINGS_CMD_SCAN:
        set_busy_locked(SETTINGS_SERVICE_OP_SCANNING, "Scanning visible networks...");
        break;
    case SETTINGS_CMD_ADD_OR_UPDATE:
        set_busy_locked(SETTINGS_SERVICE_OP_SAVING, "Saving profile...");
        break;
    case SETTINGS_CMD_REMOVE:
        set_busy_locked(SETTINGS_SERVICE_OP_REMOVING, "Removing profile...");
        break;
    default:
        xSemaphoreGive(s_mutex);
        clear_command(queued_command);
        free(queued_command);
        return ESP_ERR_INVALID_ARG;
    }

    if (xQueueSend(s_queue, &queued_command, 0) != pdTRUE) {
        finish_operation_locked(true, "Settings queue is full");
        xSemaphoreGive(s_mutex);
        publish_settings_event();
        clear_command(queued_command);
        free(queued_command);
        return ESP_ERR_TIMEOUT;
    }
    xSemaphoreGive(s_mutex);
    publish_settings_event();

    return ESP_OK;
}

esp_err_t settings_service_request_scan(void)
{
    settings_command_t command = {
        .type = SETTINGS_CMD_SCAN,
    };
    esp_err_t err;

    err = enqueue_command(&command);
    clear_command(&command);
    return err;
}

esp_err_t settings_service_request_add_or_update(const char *ssid, const char *password,
                                                 bool hidden)
{
    settings_command_t command = {
        .type = SETTINGS_CMD_ADD_OR_UPDATE,
        .hidden = hidden,
    };
    esp_err_t err;

    ESP_RETURN_ON_FALSE(ssid != NULL && ssid[0] != '\0', ESP_ERR_INVALID_ARG, TAG,
                        "ssid is required");
    ESP_RETURN_ON_FALSE(strlen(ssid) < NET_MANAGER_SSID_MAX, ESP_ERR_INVALID_ARG, TAG,
                        "ssid is too long");
    if (password != NULL) {
        ESP_RETURN_ON_FALSE(strlen(password) < NET_MANAGER_PASSWORD_MAX, ESP_ERR_INVALID_ARG, TAG,
                            "password is too long");
    }
    copy_text(command.ssid, sizeof(command.ssid), ssid);
    if (password != NULL) {
        command.password_provided = true;
        copy_text(command.password, sizeof(command.password), password);
    }
    err = enqueue_command(&command);
    clear_command(&command);
    return err;
}

esp_err_t settings_service_request_remove(const char *ssid)
{
    settings_command_t command = {
        .type = SETTINGS_CMD_REMOVE,
    };
    esp_err_t err;

    ESP_RETURN_ON_FALSE(ssid != NULL && ssid[0] != '\0', ESP_ERR_INVALID_ARG, TAG,
                        "ssid is required");
    ESP_RETURN_ON_FALSE(strlen(ssid) < NET_MANAGER_SSID_MAX, ESP_ERR_INVALID_ARG, TAG,
                        "ssid is too long");
    copy_text(command.ssid, sizeof(command.ssid), ssid);
    err = enqueue_command(&command);
    clear_command(&command);
    return err;
}
