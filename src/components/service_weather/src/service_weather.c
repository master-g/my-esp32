#include "service_weather.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "core_types/app_event.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "net_manager.h"
#include "nvs.h"
#include "power_policy.h"
#include "weather_client.h"
#include "weather_mapper.h"

static const char *TAG = "weather_service";
static weather_snapshot_t s_snapshot;
static weather_location_config_t s_location_config;
static SemaphoreHandle_t s_mutex;
static QueueHandle_t s_command_queue;
static int64_t s_last_request_us;
static int64_t s_last_success_us;
static bool s_refresh_in_progress;

#define WEATHER_REFRESH_INTERVAL_US (30LL * 60 * 1000 * 1000)
#define WEATHER_MANUAL_REFRESH_GUARD_US (60LL * 1000 * 1000)
#define WEATHER_TASK_STACK_SIZE (16 * 1024)

typedef enum {
    WEATHER_CMD_REFRESH = 1,
} weather_service_cmd_t;

static void weather_service_copy_runtime_state(weather_location_config_t *location,
                                               int64_t *last_request_us, int64_t *last_success_us,
                                               bool *refresh_in_progress, weather_state_t *state)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (location != NULL) {
        *location = s_location_config;
    }
    if (last_request_us != NULL) {
        *last_request_us = s_last_request_us;
    }
    if (last_success_us != NULL) {
        *last_success_us = s_last_success_us;
    }
    if (refresh_in_progress != NULL) {
        *refresh_in_progress = s_refresh_in_progress;
    }
    if (state != NULL) {
        *state = s_snapshot.state;
    }
    xSemaphoreGive(s_mutex);
}

static esp_err_t load_location_config_from_nvs(void)
{
    nvs_handle_t handle = 0;
    size_t required = sizeof(s_location_config.city_label);
    size_t lat_required = sizeof(s_location_config.latitude);
    size_t lon_required = sizeof(s_location_config.longitude);
    esp_err_t err = nvs_open("weather", NVS_READWRITE, &handle);

    ESP_RETURN_ON_ERROR(err, TAG, "failed to open weather namespace");

    memset(&s_location_config, 0, sizeof(s_location_config));
    err = nvs_get_str(handle, "city_label", s_location_config.city_label, &required);
    if (err == ESP_ERR_NVS_NOT_FOUND || s_location_config.city_label[0] == '\0') {
        snprintf(s_location_config.city_label, sizeof(s_location_config.city_label), "%s",
                 CONFIG_DASH_WEATHER_CITY_LABEL);
        ESP_RETURN_ON_ERROR(nvs_set_str(handle, "city_label", s_location_config.city_label), TAG,
                            "failed to seed weather city");
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_get_str(handle, "latitude", s_location_config.latitude, &lat_required);
        if (err == ESP_ERR_NVS_NOT_FOUND || s_location_config.latitude[0] == '\0') {
            snprintf(s_location_config.latitude, sizeof(s_location_config.latitude), "%s",
                     CONFIG_DASH_WEATHER_LATITUDE);
            ESP_RETURN_ON_ERROR(nvs_set_str(handle, "latitude", s_location_config.latitude), TAG,
                                "failed to seed weather latitude");
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = nvs_get_str(handle, "longitude", s_location_config.longitude, &lon_required);
        if (err == ESP_ERR_NVS_NOT_FOUND || s_location_config.longitude[0] == '\0') {
            snprintf(s_location_config.longitude, sizeof(s_location_config.longitude), "%s",
                     CONFIG_DASH_WEATHER_LONGITUDE);
            ESP_RETURN_ON_ERROR(nvs_set_str(handle, "longitude", s_location_config.longitude), TAG,
                                "failed to seed weather longitude");
            err = ESP_OK;
        }
    }

    ESP_RETURN_ON_ERROR(nvs_commit(handle), TAG, "failed to commit weather defaults");
    nvs_close(handle);
    return err;
}

static void publish_weather_event(void)
{
    app_event_t event = {
        .type = APP_EVENT_DATA_WEATHER,
        .payload = NULL,
    };

    event_bus_publish(&event);
}

static uint32_t current_epoch_s(void)
{
    time_t now = time(NULL);

    if (now <= 1700000000) {
        return 0;
    }

    return (uint32_t)now;
}

static void weather_service_mark_stale_if_needed(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_snapshot.state == WEATHER_LIVE && s_last_success_us > 0 &&
        (esp_timer_get_time() - s_last_success_us) >= WEATHER_REFRESH_INTERVAL_US) {
        s_snapshot.state = WEATHER_STALE;
    }
    xSemaphoreGive(s_mutex);
}

static void weather_service_update_success(const weather_location_config_t *location,
                                           const weather_client_result_t *result)
{
    memset(s_snapshot.text, 0, sizeof(s_snapshot.text));
    s_snapshot.state = WEATHER_LIVE;
    s_snapshot.updated_at_epoch_s = current_epoch_s();
    s_snapshot.last_error_epoch_s = 0;
    s_snapshot.temperature_c_tenths = result->temperature_c_tenths;
    s_snapshot.icon_id = weather_mapper_icon_for_code(result->weather_code, result->is_day);
    snprintf(s_snapshot.city, sizeof(s_snapshot.city), "%s",
             (location != NULL) ? location->city_label : "");
    snprintf(s_snapshot.text, sizeof(s_snapshot.text), "%s",
             weather_mapper_text_for_code(result->weather_code, result->is_day));
    s_last_success_us = esp_timer_get_time();
}

static void weather_service_update_failure(void)
{
    s_snapshot.last_error_epoch_s = current_epoch_s();
    s_snapshot.state = (s_last_success_us > 0) ? WEATHER_STALE : WEATHER_ERROR;
}

static bool weather_service_should_auto_refresh(void)
{
    power_policy_output_t policy;
    const int64_t now_us = esp_timer_get_time();
    int64_t last_request_us = 0;
    int64_t last_success_us = 0;
    bool refresh_in_progress = false;

    power_policy_get_output(&policy);
    weather_service_copy_runtime_state(NULL, &last_request_us, &last_success_us,
                                       &refresh_in_progress, NULL);
    if (!net_manager_is_connected() || !policy.weather_refresh_allowed || refresh_in_progress) {
        return false;
    }

    if (last_success_us == 0) {
        if (last_request_us == 0) {
            return true;
        }

        return (now_us - last_request_us) >= WEATHER_MANUAL_REFRESH_GUARD_US;
    }

    return (now_us - last_success_us) >= WEATHER_REFRESH_INTERVAL_US;
}

static bool weather_service_should_refresh_on_reconnect(void)
{
    power_policy_output_t policy;
    const int64_t now_us = esp_timer_get_time();
    int64_t last_request_us = 0;
    int64_t last_success_us = 0;
    bool refresh_in_progress = false;
    weather_state_t state = WEATHER_EMPTY;

    power_policy_get_output(&policy);
    weather_service_copy_runtime_state(NULL, &last_request_us, &last_success_us,
                                       &refresh_in_progress, &state);
    if (!net_manager_is_connected() || !policy.weather_refresh_allowed || refresh_in_progress) {
        return false;
    }

    if (last_success_us == 0) {
        if (last_request_us == 0) {
            return true;
        }

        return (now_us - last_request_us) >= WEATHER_MANUAL_REFRESH_GUARD_US;
    }

    if (state == WEATHER_STALE || state == WEATHER_ERROR) {
        return true;
    }

    return false;
}

static void weather_service_task(void *arg)
{
    weather_service_cmd_t cmd;

    (void)arg;
    for (;;) {
        if (xQueueReceive(s_command_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (cmd != WEATHER_CMD_REFRESH || !net_manager_is_connected()) {
            continue;
        }
        {
            bool refresh_in_progress = false;
            weather_location_config_t location = {0};

            xSemaphoreTake(s_mutex, portMAX_DELAY);
            refresh_in_progress = s_refresh_in_progress;
            if (!refresh_in_progress) {
                s_refresh_in_progress = true;
                if (s_last_success_us > 0) {
                    s_snapshot.state = WEATHER_REFRESHING;
                }
                location = s_location_config;
            }
            xSemaphoreGive(s_mutex);
            if (refresh_in_progress) {
                continue;
            }
            publish_weather_event();

            {
                weather_client_result_t result = {0};
                if (weather_client_fetch_current(&location, &result) == ESP_OK) {
                    char snapshot_text[sizeof(s_snapshot.text)] = {0};
                    int16_t temperature_c_tenths = 0;

                    xSemaphoreTake(s_mutex, portMAX_DELAY);
                    weather_service_update_success(&location, &result);
                    snprintf(snapshot_text, sizeof(snapshot_text), "%s", s_snapshot.text);
                    temperature_c_tenths = s_snapshot.temperature_c_tenths;
                    s_refresh_in_progress = false;
                    xSemaphoreGive(s_mutex);
                    ESP_LOGI(TAG, "Weather refresh ok: city=%s text=%s temp=%d.%dC",
                             location.city_label, snapshot_text, temperature_c_tenths / 10,
                             temperature_c_tenths >= 0 ? temperature_c_tenths % 10
                                                       : -(temperature_c_tenths % 10));
                } else {
                    xSemaphoreTake(s_mutex, portMAX_DELAY);
                    weather_service_update_failure();
                    s_refresh_in_progress = false;
                    xSemaphoreGive(s_mutex);
                    ESP_LOGW(TAG, "Weather refresh failed");
                }
            }
            publish_weather_event();
        }
    }
}

static void weather_service_event_handler(const app_event_t *event, void *context)
{
    weather_service_cmd_t cmd = WEATHER_CMD_REFRESH;

    (void)context;
    if (event == NULL || s_command_queue == NULL) {
        return;
    }

    if (event->type == APP_EVENT_NET_CHANGED && weather_service_should_refresh_on_reconnect()) {
        (void)xQueueSend(s_command_queue, &cmd, 0);
        return;
    }

    if (event->type == APP_EVENT_TICK_1S) {
        weather_service_mark_stale_if_needed();
        if (weather_service_should_auto_refresh()) {
            (void)xQueueSend(s_command_queue, &cmd, 0);
        }
    }
}

esp_err_t weather_service_init(void)
{
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.state = WEATHER_EMPTY;
    s_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_mutex != NULL, ESP_ERR_NO_MEM, TAG, "weather mutex alloc failed");
    ESP_RETURN_ON_ERROR(load_location_config_from_nvs(), TAG, "weather config load failed");
    snprintf(s_snapshot.city, sizeof(s_snapshot.city), "%s", s_location_config.city_label);
    s_command_queue = xQueueCreate(4, sizeof(weather_service_cmd_t));
    if (s_command_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_last_request_us = 0;
    s_last_success_us = 0;
    s_refresh_in_progress = false;
    ESP_RETURN_ON_ERROR(event_bus_subscribe(weather_service_event_handler, NULL), TAG,
                        "weather subscribe failed");
    {
        BaseType_t ret = xTaskCreatePinnedToCore(weather_service_task, "weather_service",
                                                 WEATHER_TASK_STACK_SIZE, NULL, 4, NULL, 1);
        ESP_RETURN_ON_FALSE(ret == pdPASS, ESP_ERR_NO_MEM, TAG, "weather task create failed");
    }
    return ESP_OK;
}

void weather_service_request_refresh(void)
{
    weather_service_cmd_t cmd = WEATHER_CMD_REFRESH;
    power_policy_output_t policy;
    const int64_t now_us = esp_timer_get_time();

    power_policy_get_output(&policy);
    if (!net_manager_is_connected() || !policy.weather_refresh_allowed) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_refresh_in_progress || (s_last_request_us != 0 &&
                                  (now_us - s_last_request_us) < WEATHER_MANUAL_REFRESH_GUARD_US)) {
        xSemaphoreGive(s_mutex);
        return;
    }
    s_last_request_us = now_us;
    s_snapshot.state = WEATHER_REFRESHING;
    xSemaphoreGive(s_mutex);
    publish_weather_event();
    (void)xQueueSend(s_command_queue, &cmd, 0);
}

bool weather_service_can_refresh(void)
{
    power_policy_output_t policy;
    int64_t last_request_us = 0;
    bool refresh_in_progress = false;

    power_policy_get_output(&policy);
    weather_service_copy_runtime_state(NULL, &last_request_us, NULL, &refresh_in_progress, NULL);
    if (!net_manager_is_connected() || refresh_in_progress || !policy.weather_refresh_allowed) {
        return false;
    }

    if (last_request_us == 0) {
        return true;
    }

    return (esp_timer_get_time() - last_request_us) >= WEATHER_MANUAL_REFRESH_GUARD_US;
}

void weather_service_get_location_config(weather_location_config_t *config)
{
    if (config == NULL) {
        return;
    }

    weather_service_copy_runtime_state(config, NULL, NULL, NULL, NULL);
}

esp_err_t weather_service_apply_location_config(const weather_location_config_t *config)
{
    if (config == NULL || config->city_label[0] == '\0' || config->latitude[0] == '\0' ||
        config->longitude[0] == '\0' ||
        strlen(config->city_label) >= sizeof(s_location_config.city_label) ||
        strlen(config->latitude) >= sizeof(s_location_config.latitude) ||
        strlen(config->longitude) >= sizeof(s_location_config.longitude)) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(&s_location_config, config, sizeof(s_location_config));
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.state = WEATHER_EMPTY;
    snprintf(s_snapshot.city, sizeof(s_snapshot.city), "%s", s_location_config.city_label);
    s_last_request_us = 0;
    s_last_success_us = 0;
    s_refresh_in_progress = false;
    xSemaphoreGive(s_mutex);
    publish_weather_event();

    if (net_manager_is_connected()) {
        weather_service_request_refresh();
    }

    return ESP_OK;
}

void weather_service_get_snapshot(weather_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }

    weather_service_mark_stale_if_needed();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_snapshot;
    xSemaphoreGive(s_mutex);
}
