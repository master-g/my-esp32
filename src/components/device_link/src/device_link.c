#include "device_link.h"

#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bsp_board_config.h"
#include "cJSON.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "net_manager.h"
#include "nvs.h"
#include "service_claude.h"
#include "service_time.h"
#include "service_weather.h"

#define DEVICE_LINK_PROTOCOL_PREFIX "@esp32dash "
#define DEVICE_LINK_PROTOCOL_VERSION 1
#define DEVICE_LINK_LINE_MAX 768
#define DEVICE_LINK_HELLO_INTERVAL_MS 1000

static const char *TAG = "device_link";
static SemaphoreHandle_t s_stdout_lock;
static char s_device_id[32];
static bool s_started;
static int s_usb_fd = -1;

typedef struct {
    bool set_wifi_ssid;
    bool set_wifi_password;
    bool set_timezone_name;
    bool set_timezone_tz;
    bool set_weather_city;
    bool set_weather_latitude;
    bool set_weather_longitude;
    char wifi_ssid[NET_MANAGER_SSID_MAX];
    char wifi_password[NET_MANAGER_PASSWORD_MAX];
    char timezone_name[TIME_SERVICE_TIMEZONE_NAME_MAX];
    char timezone_tz[TIME_SERVICE_TIMEZONE_TZ_MAX];
    weather_location_config_t weather;
} pending_config_t;

static const char *capabilities[] = {
    "device.info",     "device.reboot", "config.export",
    "config.set_many", "wifi.scan",     "claude.update",
};

static const char *wifi_auth_mode_to_string(uint8_t auth_mode)
{
    switch ((wifi_auth_mode_t)auth_mode) {
    case WIFI_AUTH_OPEN:
        return "open";
    case WIFI_AUTH_WEP:
        return "wep";
    case WIFI_AUTH_WPA_PSK:
        return "wpa_psk";
    case WIFI_AUTH_WPA2_PSK:
        return "wpa2_psk";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "wpa_wpa2_psk";
    case WIFI_AUTH_WPA2_ENTERPRISE:
        return "wpa2_enterprise";
    case WIFI_AUTH_WPA3_PSK:
        return "wpa3_psk";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "wpa2_wpa3_psk";
    default:
        return "unknown";
    }
}

static claude_run_state_t claude_status_from_text(const char *status)
{
    if (status == NULL) {
        return CLAUDE_RUN_UNKNOWN;
    }
    if (strcmp(status, "waiting_for_input") == 0) {
        return CLAUDE_RUN_WAITING_FOR_INPUT;
    }
    if (strcmp(status, "processing") == 0) {
        return CLAUDE_RUN_PROCESSING;
    }
    if (strcmp(status, "running_tool") == 0) {
        return CLAUDE_RUN_RUNNING_TOOL;
    }
    if (strcmp(status, "compacting") == 0) {
        return CLAUDE_RUN_COMPACTING;
    }
    if (strcmp(status, "ended") == 0) {
        return CLAUDE_RUN_ENDED;
    }
    return CLAUDE_RUN_UNKNOWN;
}

static void write_protocol_json(cJSON *root)
{
    char *json = NULL;
    char *line = NULL;
    size_t json_len = 0;
    size_t prefix_len = strlen(DEVICE_LINK_PROTOCOL_PREFIX);

    if (root == NULL) {
        return;
    }

    json = cJSON_PrintUnformatted(root);
    if (json == NULL) {
        return;
    }

    json_len = strlen(json);
    line = malloc(prefix_len + json_len + 2);
    if (line != NULL) {
        memcpy(line, DEVICE_LINK_PROTOCOL_PREFIX, prefix_len);
        memcpy(line + prefix_len, json, json_len);
        line[prefix_len + json_len] = '\n';
        line[prefix_len + json_len + 1] = '\0';
        if (s_stdout_lock != NULL) {
            (void)xSemaphoreTake(s_stdout_lock, portMAX_DELAY);
        }
        if (s_usb_fd >= 0) {
            size_t remaining = prefix_len + json_len + 1;
            const char *cursor = line;
            while (remaining > 0) {
                ssize_t rc = write(s_usb_fd, cursor, remaining);
                if (rc > 0) {
                    cursor += rc;
                    remaining -= (size_t)rc;
                    continue;
                }
                if (rc < 0 && errno == EAGAIN) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
                break;
            }
        }
        if (s_stdout_lock != NULL) {
            (void)xSemaphoreGive(s_stdout_lock);
        }
        free(line);
    }

    cJSON_free(json);
}

static void send_response_error(const char *id, const char *code, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *error = cJSON_CreateObject();

    if (root == NULL || error == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(error);
        return;
    }

    cJSON_AddStringToObject(root, "type", "response");
    cJSON_AddStringToObject(root, "id", (id != NULL) ? id : "");
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    cJSON_AddItemToObject(root, "error", error);
    write_protocol_json(root);
    cJSON_Delete(root);
}

static void send_response_ok(const char *id, cJSON *result)
{
    cJSON *root = cJSON_CreateObject();

    if (root == NULL) {
        cJSON_Delete(result);
        return;
    }

    cJSON_AddStringToObject(root, "type", "response");
    cJSON_AddStringToObject(root, "id", (id != NULL) ? id : "");
    cJSON_AddBoolToObject(root, "ok", true);
    if (result != NULL) {
        cJSON_AddItemToObject(root, "result", result);
    }
    write_protocol_json(root);
    cJSON_Delete(root);
}

static void send_hello(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *caps = cJSON_CreateArray();
    size_t i = 0;

    if (root == NULL || caps == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(caps);
        return;
    }

    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "protocol_version", DEVICE_LINK_PROTOCOL_VERSION);
    cJSON_AddStringToObject(root, "device_id", s_device_id);
    cJSON_AddStringToObject(root, "product", BSP_BOARD_NAME);
    for (i = 0; i < sizeof(capabilities) / sizeof(capabilities[0]); ++i) {
        cJSON_AddItemToArray(caps, cJSON_CreateString(capabilities[i]));
    }
    cJSON_AddItemToObject(root, "capabilities", caps);
    write_protocol_json(root);
    cJSON_Delete(root);
}

static void hello_task(void *arg)
{
    (void)arg;

    for (;;) {
        send_hello();
        vTaskDelay(pdMS_TO_TICKS(DEVICE_LINK_HELLO_INTERVAL_MS));
    }
}

static bool copy_json_string(const cJSON *object, const char *key, char *dst, size_t dst_size)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);

    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }

    snprintf(dst, dst_size, "%s", item->valuestring);
    return true;
}

static esp_err_t write_namespace_strings(const char *namespace_name, const char *const *keys,
                                         const char *const *values, const bool *writes,
                                         size_t count)
{
    nvs_handle_t handle = 0;
    esp_err_t err = ESP_OK;
    size_t i = 0;
    bool dirty = false;

    for (i = 0; i < count; ++i) {
        if (writes[i]) {
            dirty = true;
            break;
        }
    }

    if (!dirty) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(nvs_open(namespace_name, NVS_READWRITE, &handle), TAG,
                        "failed to open namespace %s", namespace_name);
    for (i = 0; i < count; ++i) {
        if (!writes[i]) {
            continue;
        }
        err = nvs_set_str(handle, keys[i], values[i]);
        if (err != ESP_OK) {
            nvs_close(handle);
            return err;
        }
    }

    err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static cJSON *build_config_export(void)
{
    net_credentials_summary_t wifi = {0};
    weather_location_config_t weather = {0};
    cJSON *result = cJSON_CreateObject();
    cJSON *items = cJSON_CreateArray();
    cJSON *item = NULL;

    if (result == NULL || items == NULL) {
        cJSON_Delete(result);
        cJSON_Delete(items);
        return NULL;
    }

    net_manager_get_credentials_summary(&wifi);
    weather_service_get_location_config(&weather);

    item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "group", "wifi");
    cJSON_AddStringToObject(item, "label", "Wi-Fi SSID");
    cJSON_AddStringToObject(item, "key", "wifi.ssid");
    cJSON_AddStringToObject(item, "value", wifi.ssid);
    cJSON_AddStringToObject(item, "value_type", "string");
    cJSON_AddBoolToObject(item, "mutable", true);
    cJSON_AddBoolToObject(item, "secret", false);
    cJSON_AddBoolToObject(item, "has_value", wifi.ssid[0] != '\0');
    cJSON_AddItemToArray(items, item);

    item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "group", "wifi");
    cJSON_AddStringToObject(item, "label", "Wi-Fi Password");
    cJSON_AddStringToObject(item, "key", "wifi.password");
    cJSON_AddNullToObject(item, "value");
    cJSON_AddStringToObject(item, "value_type", "string");
    cJSON_AddBoolToObject(item, "mutable", true);
    cJSON_AddBoolToObject(item, "secret", true);
    cJSON_AddBoolToObject(item, "has_value", wifi.has_password);
    cJSON_AddItemToArray(items, item);

    item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "group", "time");
    cJSON_AddStringToObject(item, "label", "Timezone");
    cJSON_AddStringToObject(item, "key", "time.timezone_name");
    if (time_service_get_timezone_name()[0] != '\0') {
        cJSON_AddStringToObject(item, "value", time_service_get_timezone_name());
    } else {
        cJSON_AddNullToObject(item, "value");
    }
    cJSON_AddStringToObject(item, "value_type", "string");
    cJSON_AddBoolToObject(item, "mutable", true);
    cJSON_AddBoolToObject(item, "secret", false);
    cJSON_AddBoolToObject(item, "has_value", time_service_get_timezone_name()[0] != '\0');
    cJSON_AddItemToArray(items, item);

    item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "group", "time");
    cJSON_AddStringToObject(item, "label", "POSIX TZ");
    cJSON_AddStringToObject(item, "key", "time.timezone_tz");
    cJSON_AddStringToObject(item, "value", time_service_get_timezone_tz());
    cJSON_AddStringToObject(item, "value_type", "string");
    cJSON_AddBoolToObject(item, "mutable", false);
    cJSON_AddBoolToObject(item, "secret", false);
    cJSON_AddBoolToObject(item, "has_value", time_service_get_timezone_tz()[0] != '\0');
    cJSON_AddItemToArray(items, item);

    item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "group", "weather");
    cJSON_AddStringToObject(item, "label", "Weather City");
    cJSON_AddStringToObject(item, "key", "weather.city_label");
    cJSON_AddStringToObject(item, "value", weather.city_label);
    cJSON_AddStringToObject(item, "value_type", "string");
    cJSON_AddBoolToObject(item, "mutable", true);
    cJSON_AddBoolToObject(item, "secret", false);
    cJSON_AddBoolToObject(item, "has_value", weather.city_label[0] != '\0');
    cJSON_AddItemToArray(items, item);

    item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "group", "weather");
    cJSON_AddStringToObject(item, "label", "Weather Latitude");
    cJSON_AddStringToObject(item, "key", "weather.latitude");
    cJSON_AddStringToObject(item, "value", weather.latitude);
    cJSON_AddStringToObject(item, "value_type", "string");
    cJSON_AddBoolToObject(item, "mutable", true);
    cJSON_AddBoolToObject(item, "secret", false);
    cJSON_AddBoolToObject(item, "has_value", weather.latitude[0] != '\0');
    cJSON_AddItemToArray(items, item);

    item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "group", "weather");
    cJSON_AddStringToObject(item, "label", "Weather Longitude");
    cJSON_AddStringToObject(item, "key", "weather.longitude");
    cJSON_AddStringToObject(item, "value", weather.longitude);
    cJSON_AddStringToObject(item, "value_type", "string");
    cJSON_AddBoolToObject(item, "mutable", true);
    cJSON_AddBoolToObject(item, "secret", false);
    cJSON_AddBoolToObject(item, "has_value", weather.longitude[0] != '\0');
    cJSON_AddItemToArray(items, item);

    cJSON_AddItemToObject(result, "items", items);
    return result;
}

static esp_err_t apply_pending_config(const pending_config_t *pending, cJSON **result_out)
{
    net_credentials_summary_t current_wifi = {0};
    weather_location_config_t current_weather = {0};
    char final_ssid[NET_MANAGER_SSID_MAX] = {0};
    char final_password[NET_MANAGER_PASSWORD_MAX] = {0};
    char final_timezone_name[TIME_SERVICE_TIMEZONE_NAME_MAX] = {0};
    char final_timezone_tz[TIME_SERVICE_TIMEZONE_TZ_MAX] = {0};
    bool wifi_changed = false;
    bool time_changed = false;
    bool weather_changed = false;
    cJSON *result = cJSON_CreateObject();
    cJSON *updated = cJSON_CreateArray();

    if (pending == NULL || result_out == NULL || result == NULL || updated == NULL) {
        cJSON_Delete(result);
        cJSON_Delete(updated);
        return ESP_ERR_NO_MEM;
    }

    net_manager_get_credentials_summary(&current_wifi);
    weather_service_get_location_config(&current_weather);
    snprintf(final_ssid, sizeof(final_ssid), "%s", current_wifi.ssid);
    snprintf(final_timezone_name, sizeof(final_timezone_name), "%s",
             time_service_get_timezone_name());
    snprintf(final_timezone_tz, sizeof(final_timezone_tz), "%s", time_service_get_timezone_tz());

    if (pending->set_wifi_ssid) {
        snprintf(final_ssid, sizeof(final_ssid), "%s", pending->wifi_ssid);
        wifi_changed = true;
    }
    if (pending->set_wifi_password) {
        snprintf(final_password, sizeof(final_password), "%s", pending->wifi_password);
        wifi_changed = true;
    }
    if (pending->set_wifi_ssid && !pending->set_wifi_password &&
        strcmp(final_ssid, current_wifi.ssid) == 0) {
        wifi_changed = false;
    }
    if (wifi_changed && strcmp(final_ssid, current_wifi.ssid) != 0 && !pending->set_wifi_password) {
        cJSON_Delete(result);
        cJSON_Delete(updated);
        return ESP_ERR_INVALID_ARG;
    }
    if (wifi_changed && final_ssid[0] == '\0' && final_password[0] != '\0') {
        cJSON_Delete(result);
        cJSON_Delete(updated);
        return ESP_ERR_INVALID_ARG;
    }

    if (pending->set_timezone_name) {
        snprintf(final_timezone_name, sizeof(final_timezone_name), "%s", pending->timezone_name);
        time_changed = true;
    }
    if (pending->set_timezone_tz) {
        snprintf(final_timezone_tz, sizeof(final_timezone_tz), "%s", pending->timezone_tz);
        time_changed = true;
    }
    if (time_changed && (final_timezone_name[0] == '\0' || final_timezone_tz[0] == '\0')) {
        cJSON_Delete(result);
        cJSON_Delete(updated);
        return ESP_ERR_INVALID_ARG;
    }

    if (pending->set_weather_city) {
        snprintf(current_weather.city_label, sizeof(current_weather.city_label), "%s",
                 pending->weather.city_label);
        weather_changed = true;
    }
    if (pending->set_weather_latitude) {
        snprintf(current_weather.latitude, sizeof(current_weather.latitude), "%s",
                 pending->weather.latitude);
        weather_changed = true;
    }
    if (pending->set_weather_longitude) {
        snprintf(current_weather.longitude, sizeof(current_weather.longitude), "%s",
                 pending->weather.longitude);
        weather_changed = true;
    }
    if (weather_changed &&
        (current_weather.city_label[0] == '\0' || current_weather.latitude[0] == '\0' ||
         current_weather.longitude[0] == '\0')) {
        cJSON_Delete(result);
        cJSON_Delete(updated);
        return ESP_ERR_INVALID_ARG;
    }

    if (wifi_changed) {
        const char *keys[] = {"ssid", "password"};
        const char *values[] = {final_ssid, final_password};
        bool writes[] = {pending->set_wifi_ssid, pending->set_wifi_password};
        ESP_RETURN_ON_ERROR(write_namespace_strings("wifi", keys, values, writes, 2), TAG,
                            "failed to persist wifi config");
        ESP_RETURN_ON_ERROR(net_manager_apply_credentials(
                                final_ssid, pending->set_wifi_password ? final_password : ""),
                            TAG, "failed to apply wifi config");
        if (pending->set_wifi_ssid) {
            cJSON_AddItemToArray(updated, cJSON_CreateString("wifi.ssid"));
        }
        if (pending->set_wifi_password) {
            cJSON_AddItemToArray(updated, cJSON_CreateString("wifi.password"));
        }
    }

    if (time_changed) {
        const char *keys[] = {"timezone_name", "timezone"};
        const char *values[] = {final_timezone_name, final_timezone_tz};
        bool writes[] = {pending->set_timezone_name, pending->set_timezone_tz};
        ESP_RETURN_ON_ERROR(write_namespace_strings("time", keys, values, writes, 2), TAG,
                            "failed to persist time config");
        ESP_RETURN_ON_ERROR(
            time_service_apply_timezone_config(final_timezone_name, final_timezone_tz), TAG,
            "failed to apply timezone config");
        if (pending->set_timezone_name) {
            cJSON_AddItemToArray(updated, cJSON_CreateString("time.timezone_name"));
        }
        if (pending->set_timezone_tz) {
            cJSON_AddItemToArray(updated, cJSON_CreateString("time.timezone_tz"));
        }
    }

    if (weather_changed) {
        const char *keys[] = {"city_label", "latitude", "longitude"};
        const char *values[] = {current_weather.city_label, current_weather.latitude,
                                current_weather.longitude};
        bool writes[] = {pending->set_weather_city, pending->set_weather_latitude,
                         pending->set_weather_longitude};
        ESP_RETURN_ON_ERROR(write_namespace_strings("weather", keys, values, writes, 3), TAG,
                            "failed to persist weather config");
        ESP_RETURN_ON_ERROR(weather_service_apply_location_config(&current_weather), TAG,
                            "failed to apply weather config");
        if (pending->set_weather_city) {
            cJSON_AddItemToArray(updated, cJSON_CreateString("weather.city_label"));
        }
        if (pending->set_weather_latitude) {
            cJSON_AddItemToArray(updated, cJSON_CreateString("weather.latitude"));
        }
        if (pending->set_weather_longitude) {
            cJSON_AddItemToArray(updated, cJSON_CreateString("weather.longitude"));
        }
    }

    cJSON_AddItemToObject(result, "updated_keys", updated);
    *result_out = result;
    return ESP_OK;
}

static esp_err_t parse_set_many_params(const cJSON *params, pending_config_t *pending)
{
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(params, "items");
    int i = 0;

    if (!cJSON_IsArray(items) || pending == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(pending, 0, sizeof(*pending));
    for (i = 0; i < cJSON_GetArraySize(items); ++i) {
        const cJSON *item = cJSON_GetArrayItem(items, i);
        const cJSON *key = cJSON_GetObjectItemCaseSensitive(item, "key");
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(item, "value");

        if (!cJSON_IsObject(item) || !cJSON_IsString(key) || key->valuestring == NULL) {
            return ESP_ERR_INVALID_ARG;
        }

        if (strcmp(key->valuestring, "wifi.ssid") == 0 && cJSON_IsString(value)) {
            pending->set_wifi_ssid = true;
            snprintf(pending->wifi_ssid, sizeof(pending->wifi_ssid), "%s", value->valuestring);
            continue;
        }
        if (strcmp(key->valuestring, "wifi.password") == 0 && cJSON_IsString(value)) {
            pending->set_wifi_password = true;
            snprintf(pending->wifi_password, sizeof(pending->wifi_password), "%s",
                     value->valuestring);
            continue;
        }
        if (strcmp(key->valuestring, "time.timezone_name") == 0 && cJSON_IsString(value)) {
            pending->set_timezone_name = true;
            snprintf(pending->timezone_name, sizeof(pending->timezone_name), "%s",
                     value->valuestring);
            continue;
        }
        if (strcmp(key->valuestring, "time.timezone_tz") == 0 && cJSON_IsString(value)) {
            pending->set_timezone_tz = true;
            snprintf(pending->timezone_tz, sizeof(pending->timezone_tz), "%s", value->valuestring);
            continue;
        }
        if (strcmp(key->valuestring, "weather.city_label") == 0 && cJSON_IsString(value)) {
            pending->set_weather_city = true;
            snprintf(pending->weather.city_label, sizeof(pending->weather.city_label), "%s",
                     value->valuestring);
            continue;
        }
        if (strcmp(key->valuestring, "weather.latitude") == 0 && cJSON_IsString(value)) {
            pending->set_weather_latitude = true;
            snprintf(pending->weather.latitude, sizeof(pending->weather.latitude), "%s",
                     value->valuestring);
            continue;
        }
        if (strcmp(key->valuestring, "weather.longitude") == 0 && cJSON_IsString(value)) {
            pending->set_weather_longitude = true;
            snprintf(pending->weather.longitude, sizeof(pending->weather.longitude), "%s",
                     value->valuestring);
            continue;
        }

        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static cJSON *build_wifi_scan_result(void)
{
    net_scan_ap_t aps[16] = {0};
    size_t ap_count = 0;
    size_t i = 0;
    cJSON *result = cJSON_CreateObject();
    cJSON *array = cJSON_CreateArray();

    if (result == NULL || array == NULL) {
        cJSON_Delete(result);
        cJSON_Delete(array);
        return NULL;
    }

    if (net_manager_scan_access_points(aps, sizeof(aps) / sizeof(aps[0]), &ap_count) != ESP_OK) {
        cJSON_Delete(result);
        cJSON_Delete(array);
        return NULL;
    }

    for (i = 0; i < ap_count; ++i) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", aps[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", aps[i].rssi);
        cJSON_AddStringToObject(item, "auth_mode", wifi_auth_mode_to_string(aps[i].auth_mode));
        cJSON_AddBoolToObject(item, "auth_required", aps[i].auth_mode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(array, item);
    }

    cJSON_AddItemToObject(result, "aps", array);
    return result;
}

static cJSON *build_device_info(void)
{
    net_snapshot_t net;
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON *result = NULL;

    net_manager_get_snapshot(&net);
    result = cJSON_CreateObject();

    if (result == NULL) {
        return NULL;
    }

    cJSON_AddStringToObject(result, "device_id", s_device_id);
    cJSON_AddStringToObject(result, "product", BSP_BOARD_NAME);
    cJSON_AddNumberToObject(result, "protocol_version", DEVICE_LINK_PROTOCOL_VERSION);
    cJSON_AddStringToObject(result, "firmware_version", app->version);
    cJSON_AddStringToObject(result, "wifi_state",
                            (net.state == NET_STATE_UP)           ? "up"
                            : (net.state == NET_STATE_CONNECTING) ? "connecting"
                                                                   : "down");
    cJSON_AddStringToObject(result, "ssid", net.ssid);
    cJSON_AddStringToObject(result, "ip_addr", net.ip_addr);
    cJSON_AddBoolToObject(result, "has_credentials", net.has_credentials);
    return result;
}

static esp_err_t parse_claude_update(const cJSON *payload, claude_snapshot_t *snapshot)
{
    const cJSON *seq = cJSON_GetObjectItemCaseSensitive(payload, "seq");
    const cJSON *unread = cJSON_GetObjectItemCaseSensitive(payload, "unread");
    const cJSON *ts = cJSON_GetObjectItemCaseSensitive(payload, "ts");
    char status[32] = {0};

    if (!cJSON_IsObject(payload) || snapshot == NULL || !cJSON_IsNumber(seq) ||
        !cJSON_IsBool(unread) || !cJSON_IsNumber(ts)) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->seq = (uint32_t)seq->valuedouble;
    snapshot->unread = cJSON_IsTrue(unread);
    snapshot->updated_at_epoch_s = (uint32_t)ts->valuedouble;
    copy_json_string(payload, "session_id", snapshot->session_id, sizeof(snapshot->session_id));
    copy_json_string(payload, "workspace", snapshot->workspace, sizeof(snapshot->workspace));
    copy_json_string(payload, "title", snapshot->title, sizeof(snapshot->title));
    copy_json_string(payload, "detail", snapshot->detail, sizeof(snapshot->detail));
    copy_json_string(payload, "event", snapshot->event, sizeof(snapshot->event));
    copy_json_string(payload, "permission_mode", snapshot->permission_mode,
                     sizeof(snapshot->permission_mode));
    copy_json_string(payload, "status", status, sizeof(status));
    snapshot->run_state = claude_status_from_text(status);
    return ESP_OK;
}

static void handle_event_frame(const cJSON *root)
{
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(root, "method");
    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");

    if (!cJSON_IsString(method) || method->valuestring == NULL) {
        return;
    }

    if (strcmp(method->valuestring, "claude.update") == 0) {
        claude_snapshot_t snapshot = {0};
        if (parse_claude_update(payload, &snapshot) == ESP_OK) {
            claude_service_apply_remote_snapshot(&snapshot);
        }
    }
}

static void handle_request_frame(const cJSON *root)
{
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(root, "method");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");

    if (!cJSON_IsString(id) || id->valuestring == NULL || !cJSON_IsString(method) ||
        method->valuestring == NULL) {
        send_response_error("", "invalid_request", "request is missing id or method");
        return;
    }

    if (strcmp(method->valuestring, "device.info") == 0) {
        send_response_ok(id->valuestring, build_device_info());
        return;
    }

    if (strcmp(method->valuestring, "device.reboot") == 0) {
        cJSON *result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "restarting", true);
        send_response_ok(id->valuestring, result);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
        return;
    }

    if (strcmp(method->valuestring, "config.export") == 0) {
        send_response_ok(id->valuestring, build_config_export());
        return;
    }

    if (strcmp(method->valuestring, "config.set_many") == 0) {
        pending_config_t pending = {0};
        cJSON *result = NULL;
        esp_err_t err = parse_set_many_params(params, &pending);
        if (err == ESP_OK) {
            err = apply_pending_config(&pending, &result);
        }
        if (err != ESP_OK) {
            send_response_error(id->valuestring, "invalid_config", esp_err_to_name(err));
            cJSON_Delete(result);
            return;
        }
        send_response_ok(id->valuestring, result);
        return;
    }

    if (strcmp(method->valuestring, "wifi.scan") == 0) {
        cJSON *result = build_wifi_scan_result();
        if (result == NULL) {
            send_response_error(id->valuestring, "scan_failed", "Wi-Fi scan failed");
            return;
        }
        send_response_ok(id->valuestring, result);
        return;
    }

    send_response_error(id->valuestring, "unknown_method", method->valuestring);
}

static void process_protocol_line(const char *line)
{
    cJSON *root = NULL;
    const cJSON *type = NULL;

    if (strncmp(line, DEVICE_LINK_PROTOCOL_PREFIX, strlen(DEVICE_LINK_PROTOCOL_PREFIX)) != 0) {
        return;
    }

    root = cJSON_Parse(line + strlen(DEVICE_LINK_PROTOCOL_PREFIX));
    if (root == NULL) {
        return;
    }

    type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (cJSON_IsString(type) && type->valuestring != NULL) {
        if (strcmp(type->valuestring, "request") == 0) {
            handle_request_frame(root);
        } else if (strcmp(type->valuestring, "event") == 0) {
            handle_event_frame(root);
        }
    }

    cJSON_Delete(root);
}

static void reader_task(void *arg)
{
    char line[DEVICE_LINK_LINE_MAX];
    uint8_t buffer[128];
    size_t line_len = 0;

    (void)arg;

    for (;;) {
        int bytes_read = (s_usb_fd >= 0) ? (int)read(s_usb_fd, buffer, sizeof(buffer)) : -1;
        int i = 0;

        if (bytes_read < 0) {
            if (errno == EAGAIN) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (bytes_read == 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        for (i = 0; i < bytes_read; ++i) {
            char ch = (char)buffer[i];

            if (ch == '\r') {
                continue;
            }
            if (ch == '\n') {
                line[line_len] = '\0';
                if (line_len > 0) {
                    process_protocol_line(line);
                }
                line_len = 0;
                continue;
            }

            if (line_len + 1 < sizeof(line)) {
                line[line_len++] = ch;
            } else {
                line_len = 0;
            }
        }
    }
}

esp_err_t device_link_init(void)
{
    uint8_t mac[6] = {0};
    usb_serial_jtag_driver_config_t usb_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();

    if (s_started) {
        return ESP_OK;
    }

    s_stdout_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_stdout_lock != NULL, ESP_ERR_NO_MEM, TAG, "failed to create stdout lock");
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_STA), TAG, "failed to read device mac");
    if (!usb_serial_jtag_is_driver_installed()) {
        ESP_RETURN_ON_ERROR(usb_serial_jtag_driver_install(&usb_config), TAG,
                            "failed to install usb_serial_jtag driver");
    }
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF);
    ESP_RETURN_ON_ERROR(usb_serial_jtag_vfs_register(), TAG,
                        "failed to register usb_serial_jtag vfs");
    usb_serial_jtag_vfs_use_driver();
    s_usb_fd = open("/dev/usbserjtag", O_RDWR | O_NONBLOCK);
    ESP_RETURN_ON_FALSE(s_usb_fd >= 0, ESP_FAIL, TAG, "failed to open /dev/usbserjtag");
    snprintf(s_device_id, sizeof(s_device_id), "esp32-dashboard-%02x%02x%02x", mac[3], mac[4],
             mac[5]);

    {
        BaseType_t ret;
        ret = xTaskCreatePinnedToCore(reader_task, "device_link_rx", 6144, NULL, 4, NULL, 1);
        ESP_RETURN_ON_FALSE(ret == pdPASS, ESP_ERR_NO_MEM, TAG, "reader task create failed");
        ret = xTaskCreatePinnedToCore(hello_task, "device_link_hello", 4096, NULL, 2, NULL, 1);
        ESP_RETURN_ON_FALSE(ret == pdPASS, ESP_ERR_NO_MEM, TAG, "hello task create failed");
    }
    s_started = true;
    return ESP_OK;
}
