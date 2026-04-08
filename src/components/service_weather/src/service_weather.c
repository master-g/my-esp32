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
#include "freertos/task.h"
#include "net_manager.h"
#include "nvs.h"
#include "power_policy.h"
#include "weather_client.h"
#include "weather_mapper.h"

static const char *TAG = "weather_service";
static weather_snapshot_t s_snapshot;
static weather_location_config_t s_location_config;
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
        .payload = &s_snapshot,
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
    if (s_snapshot.state == WEATHER_LIVE && s_last_success_us > 0 &&
        (esp_timer_get_time() - s_last_success_us) >= WEATHER_REFRESH_INTERVAL_US) {
        s_snapshot.state = WEATHER_STALE;
    }
}

static void weather_service_update_success(const weather_client_result_t *result)
{
    memset(s_snapshot.text, 0, sizeof(s_snapshot.text));
    s_snapshot.state = WEATHER_LIVE;
    s_snapshot.updated_at_epoch_s = current_epoch_s();
    s_snapshot.last_error_epoch_s = 0;
    s_snapshot.temperature_c_tenths = result->temperature_c_tenths;
    s_snapshot.icon_id = weather_mapper_icon_for_code(result->weather_code, result->is_day);
    snprintf(s_snapshot.city, sizeof(s_snapshot.city), "%s", s_location_config.city_label);
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
    const power_policy_output_t *policy = power_policy_get_output();
    const int64_t now_us = esp_timer_get_time();

    if (!net_manager_is_connected() || !policy->weather_refresh_allowed || s_refresh_in_progress) {
        return false;
    }

    if (s_last_success_us == 0) {
        if (s_last_request_us == 0) {
            return true;
        }

        return (now_us - s_last_request_us) >= WEATHER_MANUAL_REFRESH_GUARD_US;
    }

    return (now_us - s_last_success_us) >= WEATHER_REFRESH_INTERVAL_US;
}

static bool weather_service_should_refresh_on_reconnect(void)
{
    const power_policy_output_t *policy = power_policy_get_output();
    const int64_t now_us = esp_timer_get_time();

    if (!net_manager_is_connected() || !policy->weather_refresh_allowed || s_refresh_in_progress) {
        return false;
    }

    if (s_last_success_us == 0) {
        if (s_last_request_us == 0) {
            return true;
        }

        return (now_us - s_last_request_us) >= WEATHER_MANUAL_REFRESH_GUARD_US;
    }

    if (s_snapshot.state == WEATHER_STALE || s_snapshot.state == WEATHER_ERROR) {
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

        if (cmd != WEATHER_CMD_REFRESH || !net_manager_is_connected() || s_refresh_in_progress) {
            continue;
        }

        s_refresh_in_progress = true;
        s_snapshot.state = (s_last_success_us > 0) ? WEATHER_REFRESHING : s_snapshot.state;
        publish_weather_event();

        {
            weather_client_result_t result = {0};
            if (weather_client_fetch_current(&s_location_config, &result) == ESP_OK) {
                weather_service_update_success(&result);
                ESP_LOGI(TAG, "Weather refresh ok: city=%s text=%s temp=%d.%dC", s_snapshot.city,
                         s_snapshot.text, s_snapshot.temperature_c_tenths / 10,
                         s_snapshot.temperature_c_tenths >= 0
                             ? s_snapshot.temperature_c_tenths % 10
                             : -(s_snapshot.temperature_c_tenths % 10));
            } else {
                weather_service_update_failure();
                ESP_LOGW(TAG, "Weather refresh failed");
            }
        }

        s_refresh_in_progress = false;
        publish_weather_event();
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
    xTaskCreatePinnedToCore(weather_service_task, "weather_service", WEATHER_TASK_STACK_SIZE, NULL,
                            4, NULL, 1);
    return ESP_OK;
}

void weather_service_request_refresh(void)
{
    weather_service_cmd_t cmd = WEATHER_CMD_REFRESH;

    if (!weather_service_can_refresh()) {
        return;
    }

    s_last_request_us = esp_timer_get_time();
    s_snapshot.state = WEATHER_REFRESHING;
    publish_weather_event();
    (void)xQueueSend(s_command_queue, &cmd, 0);
}

bool weather_service_can_refresh(void)
{
    if (!net_manager_is_connected() || s_refresh_in_progress ||
        !power_policy_get_output()->weather_refresh_allowed) {
        return false;
    }

    if (s_last_request_us == 0) {
        return true;
    }

    return (esp_timer_get_time() - s_last_request_us) >= WEATHER_MANUAL_REFRESH_GUARD_US;
}

void weather_service_get_location_config(weather_location_config_t *config)
{
    if (config == NULL) {
        return;
    }

    memcpy(config, &s_location_config, sizeof(*config));
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

    memcpy(&s_location_config, config, sizeof(s_location_config));
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.state = WEATHER_EMPTY;
    snprintf(s_snapshot.city, sizeof(s_snapshot.city), "%s", s_location_config.city_label);
    s_last_request_us = 0;
    s_last_success_us = 0;
    s_refresh_in_progress = false;
    publish_weather_event();

    if (net_manager_is_connected()) {
        weather_service_request_refresh();
    }

    return ESP_OK;
}

const weather_snapshot_t *weather_service_get_snapshot(void)
{
    weather_service_mark_stale_if_needed();
    return &s_snapshot;
}
