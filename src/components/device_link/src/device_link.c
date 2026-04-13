#include "device_link.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_manager.h"
#include "bsp_board_config.h"
#include "cJSON.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "net_manager.h"
#include "nvs.h"
#include "power_policy.h"
#include "service_claude.h"
#include "service_market.h"
#include "service_time.h"
#include "service_weather.h"
#include "service_bitcoin.h"

#include "mbedtls/platform_util.h"
#include "mbedtls/base64.h"

#include "event_bus.h"

#define DEVICE_LINK_PROTOCOL_PREFIX "@esp32dash "
#define DEVICE_LINK_PROTOCOL_VERSION 1
#define DEVICE_LINK_LINE_MAX 768
#define DEVICE_LINK_HELLO_INTERVAL_MS 1000
#define DEVICE_LINK_WRITE_TIMEOUT_MS 20
#define DEVICE_LINK_WRITE_CHUNK_BYTES 128
#define APPROVAL_TIMEOUT_MS 300000
#define APPROVAL_RPC_ID_MAX 64
#define APPROVAL_WAIT_TASK_STACK 4096
#define UI_CONTROL_TIMEOUT_MS 2000
#define SCREENSHOT_TIMEOUT_MS 5000
#define SCREENSHOT_BUFFER_BYTES ((size_t)BSP_LCD_H_RES * BSP_LCD_V_RES * sizeof(uint16_t))
#define SCREENSHOT_CHUNK_RAW_BYTES 1024U
#define SCREENSHOT_CHUNK_BASE64_BYTES ((((SCREENSHOT_CHUNK_RAW_BYTES + 2U) / 3U) * 4U) + 4U)

static const char *TAG = "device_link";
static SemaphoreHandle_t s_stdout_lock;
static char s_device_id[32];
static bool s_started;
static uint32_t s_screenshot_seq;

/* Approval state — shared between reader task and LVGL task */
static SemaphoreHandle_t s_approval_sem;
static approval_request_t s_approval_req;
static approval_decision_t s_approval_decision;
static bool s_approval_uses_rpc;
static char s_approval_rpc_id[APPROVAL_RPC_ID_MAX];

static void reset_approval_state(void);

typedef struct {
    bool set_timezone_name;
    bool set_timezone_tz;
    bool set_weather_city;
    bool set_weather_latitude;
    bool set_weather_longitude;
    char timezone_name[TIME_SERVICE_TIMEZONE_NAME_MAX];
    char timezone_tz[TIME_SERVICE_TIMEZONE_TZ_MAX];
    weather_location_config_t weather;
} pending_config_t;

static const char *capabilities[] = {
    "device.info",
    "device.reboot",
    "config.export",
    "config.set_many",
    "wifi.scan",
    "wifi.profiles.list",
    "wifi.profile.add",
    "wifi.profile.remove",
    "claude.update",
    "claude.heartbeat",
    "claude.approve",
    "claude.approval.request",
    "claude.approval.dismiss",
    "claude.approval.resolved",
    "home.screensaver",
    "app.switch",
    "screen.capture.start",
    "debug.market_snapshot",
    "slot.export_hit",
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
    size_t line_len = 0;

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
        size_t written = 0;

        memcpy(line, DEVICE_LINK_PROTOCOL_PREFIX, prefix_len);
        memcpy(line + prefix_len, json, json_len);
        line[prefix_len + json_len] = '\n';
        line[prefix_len + json_len + 1] = '\0';
        line_len = prefix_len + json_len + 1;
        if (s_stdout_lock != NULL) {
            (void)xSemaphoreTake(s_stdout_lock, portMAX_DELAY);
        }

        /* Direct CLI calls connect only long enough to issue one command.
         * Periodic hello frames must not spin forever once the host closes the port. */
        if (usb_serial_jtag_is_connected()) {
            while (written < line_len) {
                size_t chunk_len = line_len - written;
                int rc;

                if (chunk_len > DEVICE_LINK_WRITE_CHUNK_BYTES) {
                    chunk_len = DEVICE_LINK_WRITE_CHUNK_BYTES;
                }

                rc = usb_serial_jtag_write_bytes(line + written, chunk_len,
                                                 pdMS_TO_TICKS(DEVICE_LINK_WRITE_TIMEOUT_MS));
                if (rc <= 0) {
                    ESP_LOGD(TAG, "protocol tx dropped after %u/%u bytes", (unsigned)written,
                             (unsigned)line_len);
                    break;
                }

                written += (size_t)rc;
            }

            if (written == line_len) {
                (void)usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(DEVICE_LINK_WRITE_TIMEOUT_MS));
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

static void send_event_frame(const char *method, cJSON *payload)
{
    cJSON *root = cJSON_CreateObject();

    if (root == NULL) {
        cJSON_Delete(payload);
        return;
    }

    cJSON_AddStringToObject(root, "type", "event");
    cJSON_AddStringToObject(root, "method", (method != NULL) ? method : "");
    if (payload != NULL) {
        cJSON_AddItemToObject(root, "payload", payload);
    }
    write_protocol_json(root);
    cJSON_Delete(root);
}

static const char *screenshot_source_to_string(app_screenshot_source_t source)
{
    switch (source) {
    case APP_SCREENSHOT_SOURCE_HOME_DIRECT:
        return "home_direct";
    case APP_SCREENSHOT_SOURCE_LVGL:
    default:
        return "lvgl";
    }
}

static cJSON *build_screenshot_start_result(const char *capture_id, const app_screenshot_t *capture)
{
    cJSON *result = cJSON_CreateObject();
    uint32_t chunk_count;

    if (result == NULL || capture == NULL || capture_id == NULL) {
        cJSON_Delete(result);
        return NULL;
    }

    chunk_count = (uint32_t)((capture->data_size + SCREENSHOT_CHUNK_RAW_BYTES - 1U) /
                             SCREENSHOT_CHUNK_RAW_BYTES);
    cJSON_AddStringToObject(result, "capture_id", capture_id);
    cJSON_AddStringToObject(result, "app", app_id_to_string(capture->info.app_id));
    cJSON_AddStringToObject(result, "source", screenshot_source_to_string(capture->info.source));
    cJSON_AddStringToObject(result, "format", "rgb565_le");
    cJSON_AddNumberToObject(result, "width", capture->info.width);
    cJSON_AddNumberToObject(result, "height", capture->info.height);
    cJSON_AddNumberToObject(result, "stride_bytes", capture->info.stride_bytes);
    cJSON_AddNumberToObject(result, "data_size", (double)capture->data_size);
    cJSON_AddNumberToObject(result, "chunk_bytes", SCREENSHOT_CHUNK_RAW_BYTES);
    cJSON_AddNumberToObject(result, "chunk_count", chunk_count);
    return result;
}

static void send_screenshot_error_event(const char *capture_id, const char *message)
{
    cJSON *payload = cJSON_CreateObject();

    if (payload == NULL) {
        return;
    }

    cJSON_AddStringToObject(payload, "capture_id", (capture_id != NULL) ? capture_id : "");
    cJSON_AddStringToObject(payload, "message", (message != NULL) ? message : "unknown error");
    send_event_frame("screen.capture.error", payload);
}

static esp_err_t stream_screenshot_capture(const char *capture_id, const app_screenshot_t *capture)
{
    char encoded[SCREENSHOT_CHUNK_BASE64_BYTES];
    size_t offset = 0;
    uint32_t index = 0;

    ESP_RETURN_ON_FALSE(capture_id != NULL, ESP_ERR_INVALID_ARG, TAG, "capture id is required");
    ESP_RETURN_ON_FALSE(capture != NULL, ESP_ERR_INVALID_ARG, TAG, "capture is required");
    ESP_RETURN_ON_FALSE(capture->buffer != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "capture buffer is required");

    while (offset < capture->data_size) {
        const size_t chunk_bytes = ((capture->data_size - offset) > SCREENSHOT_CHUNK_RAW_BYTES)
                                       ? SCREENSHOT_CHUNK_RAW_BYTES
                                       : (capture->data_size - offset);
        size_t encoded_len = 0;
        cJSON *payload = NULL;
        int ret = mbedtls_base64_encode((unsigned char *)encoded, sizeof(encoded), &encoded_len,
                                        capture->buffer + offset, chunk_bytes);

        if (ret != 0 || encoded_len >= sizeof(encoded)) {
            return ESP_FAIL;
        }

        encoded[encoded_len] = '\0';
        payload = cJSON_CreateObject();
        if (payload == NULL) {
            return ESP_ERR_NO_MEM;
        }

        cJSON_AddStringToObject(payload, "capture_id", capture_id);
        cJSON_AddNumberToObject(payload, "index", index);
        cJSON_AddStringToObject(payload, "data", encoded);
        send_event_frame("screen.capture.chunk", payload);
        offset += chunk_bytes;
        index++;
    }

    {
        cJSON *payload = cJSON_CreateObject();

        if (payload == NULL) {
            return ESP_ERR_NO_MEM;
        }

        cJSON_AddStringToObject(payload, "capture_id", capture_id);
        cJSON_AddNumberToObject(payload, "chunks_sent", index);
        cJSON_AddNumberToObject(payload, "bytes_sent", (double)capture->data_size);
        send_event_frame("screen.capture.done", payload);
    }

    return ESP_OK;
}

static void publish_ui_event(app_event_type_t type)
{
    app_event_t evt = {.type = type, .payload = NULL};
    event_bus_publish(&evt);
}

static esp_err_t focus_home_for_approval(void)
{
    return app_manager_request_switch_to(APP_ID_HOME, UI_CONTROL_TIMEOUT_MS);
}

static void send_approval_rpc_response(approval_decision_t decision)
{
    const char *decision_str = "deny";
    cJSON *result = NULL;

    if (s_approval_rpc_id[0] == '\0') {
        return;
    }

    if (decision == APPROVAL_DECISION_ALLOW) {
        decision_str = "allow";
    } else if (decision == APPROVAL_DECISION_YOLO) {
        decision_str = "yolo";
    }

    result = cJSON_CreateObject();
    if (result == NULL) {
        return;
    }

    cJSON_AddStringToObject(result, "decision", decision_str);
    send_response_ok(s_approval_rpc_id, result);
}

static void approval_rpc_wait_task(void *arg)
{
    const BaseType_t got_decision =
        xSemaphoreTake(s_approval_sem, pdMS_TO_TICKS(APPROVAL_TIMEOUT_MS));
    approval_decision_t decision = APPROVAL_DECISION_DENY;

    (void)arg;

    if (got_decision == pdTRUE) {
        decision = s_approval_decision;
    } else {
        ESP_LOGW(TAG, "claude.approve: timed out waiting for user");
    }

    send_approval_rpc_response(decision);
    reset_approval_state();
    vTaskDelete(NULL);
}

static void reset_approval_state(void)
{
    memset(&s_approval_req, 0, sizeof(s_approval_req));
    s_approval_decision = APPROVAL_DECISION_DENY;
    s_approval_uses_rpc = false;
    s_approval_rpc_id[0] = '\0';
}

static esp_err_t set_pending_approval(const cJSON *req_id, const cJSON *tool, const cJSON *desc,
                                      bool via_rpc)
{
    ESP_RETURN_ON_FALSE(cJSON_IsString(req_id) && req_id->valuestring != NULL, ESP_ERR_INVALID_ARG,
                        TAG, "approval request missing id");

    reset_approval_state();
    if (s_approval_sem != NULL) {
        (void)xSemaphoreTake(s_approval_sem, 0);
    }
    strlcpy(s_approval_req.id, req_id->valuestring, sizeof(s_approval_req.id));
    if (cJSON_IsString(tool)) {
        strlcpy(s_approval_req.tool_name, tool->valuestring, sizeof(s_approval_req.tool_name));
    }
    if (cJSON_IsString(desc)) {
        strlcpy(s_approval_req.description, desc->valuestring, sizeof(s_approval_req.description));
    }
    s_approval_req.pending = true;
    s_approval_uses_rpc = via_rpc;
    return ESP_OK;
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
    weather_location_config_t weather = {0};
    cJSON *result = cJSON_CreateObject();
    cJSON *items = cJSON_CreateArray();
    cJSON *item = NULL;

    if (result == NULL || items == NULL) {
        cJSON_Delete(result);
        cJSON_Delete(items);
        return NULL;
    }

    weather_service_get_location_config(&weather);

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
    weather_location_config_t current_weather = {0};
    char final_timezone_name[TIME_SERVICE_TIMEZONE_NAME_MAX] = {0};
    char final_timezone_tz[TIME_SERVICE_TIMEZONE_TZ_MAX] = {0};
    bool time_changed = false;
    bool weather_changed = false;
    cJSON *result = cJSON_CreateObject();
    cJSON *updated = cJSON_CreateArray();

    if (pending == NULL || result_out == NULL || result == NULL || updated == NULL) {
        cJSON_Delete(result);
        cJSON_Delete(updated);
        return ESP_ERR_NO_MEM;
    }

    weather_service_get_location_config(&current_weather);
    snprintf(final_timezone_name, sizeof(final_timezone_name), "%s",
             time_service_get_timezone_name());
    snprintf(final_timezone_tz, sizeof(final_timezone_tz), "%s", time_service_get_timezone_tz());

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

static cJSON *build_wifi_profiles_result(void)
{
    net_profile_summary_t profiles[NET_PROFILE_MAX] = {0};
    size_t profile_count = 0;
    size_t i = 0;
    cJSON *result = cJSON_CreateObject();
    cJSON *array = cJSON_CreateArray();

    if (result == NULL || array == NULL) {
        cJSON_Delete(result);
        cJSON_Delete(array);
        return NULL;
    }

    net_manager_list_profiles(profiles, sizeof(profiles) / sizeof(profiles[0]), &profile_count);
    for (i = 0; i < profile_count; ++i) {
        cJSON *item = cJSON_CreateObject();

        cJSON_AddStringToObject(item, "ssid", profiles[i].ssid);
        cJSON_AddBoolToObject(item, "hidden", profiles[i].hidden);
        cJSON_AddBoolToObject(item, "has_password", profiles[i].has_password);
        cJSON_AddBoolToObject(item, "active", profiles[i].active);
        cJSON_AddItemToArray(array, item);
    }

    cJSON_AddItemToObject(result, "profiles", array);
    return result;
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
        cJSON_AddBoolToObject(item, "auth_required", net_manager_scan_ap_auth_required(&aps[i]));
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

static const char *market_state_to_string(trading_data_state_t state)
{
    switch (state) {
    case TRADING_DATA_LOADING:
        return "loading";
    case TRADING_DATA_LIVE:
        return "live";
    case TRADING_DATA_STALE:
        return "stale";
    case TRADING_DATA_ERROR:
        return "error";
    case TRADING_DATA_EMPTY:
    default:
        return "empty";
    }
}

static const char *refresh_mode_to_string(refresh_mode_t mode)
{
    switch (mode) {
    case REFRESH_MODE_REALTIME:
        return "realtime";
    case REFRESH_MODE_INTERACTIVE_POLL:
        return "interactive_poll";
    case REFRESH_MODE_BACKGROUND_CACHE:
        return "background_cache";
    case REFRESH_MODE_PAUSED:
    default:
        return "paused";
    }
}

static bool parse_app_id_value(const char *value, app_id_t *out)
{
    if (value == NULL || out == NULL) {
        return false;
    }

    if (strcmp(value, "home") == 0) {
        *out = APP_ID_HOME;
        return true;
    }
    if (strcmp(value, "trading") == 0) {
        *out = APP_ID_TRADING;
        return true;
    }
    if (strcmp(value, "slot") == 0 || strcmp(value, "satoshi_slot") == 0) {
        *out = APP_ID_SATOSHI_SLOT;
        return true;
    }
    if (strcmp(value, "settings") == 0) {
        *out = APP_ID_SETTINGS;
        return true;
    }

    return false;
}

static cJSON *build_market_snapshot_debug(void)
{
    market_snapshot_t market = {0};
    power_policy_input_t policy_input = {0};
    power_policy_output_t policy_output = {0};
    cJSON *result = cJSON_CreateObject();

    if (result == NULL) {
        return NULL;
    }

    market_service_get_snapshot(&market);
    power_policy_get_input(&policy_input);
    power_policy_get_output(&policy_output);

    cJSON_AddStringToObject(result, "state", market_state_to_string(market.state));
    cJSON_AddBoolToObject(result, "wifi_connected", market.wifi_connected);
    cJSON_AddStringToObject(result, "pair", market.pair_label);
    cJSON_AddStringToObject(result, "price_text", market.price_text);
    cJSON_AddStringToObject(result, "change_text", market.change_text);
    cJSON_AddBoolToObject(result, "has_chart_data", market.has_chart_data);
    cJSON_AddNumberToObject(result, "summary_updated_at_epoch_s",
                            market.summary_updated_at_epoch_s);
    cJSON_AddNumberToObject(result, "chart_updated_at_epoch_s", market.chart_updated_at_epoch_s);
    cJSON_AddStringToObject(result, "active_source", market_source_label(market.active_source));
    cJSON_AddBoolToObject(result, "fallback_active", market.fallback_active);
    cJSON_AddNumberToObject(result, "source_error_count", market.source_error_count);
    cJSON_AddStringToObject(result, "foreground_app",
                            app_id_to_string(app_manager_get_foreground_app()));
    cJSON_AddStringToObject(result, "display_state",
                            (policy_input.display_state == DISPLAY_STATE_ACTIVE) ? "active"
                            : (policy_input.display_state == DISPLAY_STATE_DIM)  ? "dim"
                                                                                 : "sleep");
    cJSON_AddStringToObject(result, "market_mode",
                            refresh_mode_to_string(policy_output.market_mode));
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
        esp_err_t err = parse_claude_update(payload, &snapshot);
        if (err == ESP_OK) {
            claude_service_apply_remote_snapshot(&snapshot);
        } else {
            ESP_LOGW(TAG, "claude.update: parse failed (0x%x)", (unsigned)err);
        }
        return;
    }

    if (strcmp(method->valuestring, "claude.heartbeat") == 0) {
        claude_service_note_transport_alive();
        return;
    }

    if (strcmp(method->valuestring, "claude.approval.request") == 0) {
        const cJSON *tool = cJSON_GetObjectItemCaseSensitive(payload, "tool_name");
        const cJSON *desc = cJSON_GetObjectItemCaseSensitive(payload, "description");
        const cJSON *req_id = cJSON_GetObjectItemCaseSensitive(payload, "id");
        esp_err_t err;

        if (s_approval_req.pending) {
            ESP_LOGW(TAG, "approval request ignored because another approval is pending");
            return;
        }
        if (set_pending_approval(req_id, tool, desc, false) != ESP_OK) {
            ESP_LOGW(TAG, "claude.approval.request: invalid payload");
            return;
        }
        err = focus_home_for_approval();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "claude.approval.request: failed to focus home: %s",
                     esp_err_to_name(err));
            device_link_resolve_approval(APPROVAL_DECISION_DENY);
            return;
        }
        publish_ui_event(APP_EVENT_PERMISSION_REQUEST);
        return;
    }

    if (strcmp(method->valuestring, "claude.approval.dismiss") == 0) {
        const cJSON *req_id = cJSON_GetObjectItemCaseSensitive(payload, "id");
        if (!s_approval_req.pending || !cJSON_IsString(req_id) || req_id->valuestring == NULL) {
            return;
        }
        if (strcmp(s_approval_req.id, req_id->valuestring) != 0) {
            ESP_LOGW(TAG, "approval dismiss ignored for unknown id");
            return;
        }
        device_link_dismiss_approval();
        publish_ui_event(APP_EVENT_PERMISSION_DISMISS);
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

    if (strcmp(method->valuestring, "wifi.profiles.list") == 0) {
        cJSON *result = build_wifi_profiles_result();
        if (result == NULL) {
            send_response_error(id->valuestring, "list_failed", "Wi-Fi profile list failed");
            return;
        }
        send_response_ok(id->valuestring, result);
        return;
    }

    if (strcmp(method->valuestring, "wifi.profile.add") == 0) {
        const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(params, "ssid");
        const cJSON *password = cJSON_GetObjectItemCaseSensitive(params, "password");
        const cJSON *hidden = cJSON_GetObjectItemCaseSensitive(params, "hidden");
        const char *password_value = NULL;
        esp_err_t err;

        if (!cJSON_IsString(ssid) || ssid->valuestring == NULL) {
            send_response_error(id->valuestring, "invalid_request", "ssid is required");
            return;
        }
        if (password != NULL && !cJSON_IsString(password) && !cJSON_IsNull(password)) {
            send_response_error(id->valuestring, "invalid_request",
                                "password must be a string when provided");
            return;
        }

        if (cJSON_IsString(password)) {
            password_value = password->valuestring;
        }

        err = net_manager_add_or_update_profile(ssid->valuestring, password_value,
                                                cJSON_IsTrue(hidden));
        if (err != ESP_OK) {
            send_response_error(id->valuestring, "profile_add_failed", esp_err_to_name(err));
            return;
        }
        send_response_ok(id->valuestring, cJSON_CreateObject());
        return;
    }

    if (strcmp(method->valuestring, "wifi.profile.remove") == 0) {
        const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(params, "ssid");
        esp_err_t err;

        if (!cJSON_IsString(ssid) || ssid->valuestring == NULL) {
            send_response_error(id->valuestring, "invalid_request", "ssid is required");
            return;
        }

        err = net_manager_remove_profile(ssid->valuestring);
        if (err != ESP_OK) {
            send_response_error(id->valuestring, "profile_remove_failed", esp_err_to_name(err));
            return;
        }
        send_response_ok(id->valuestring, cJSON_CreateObject());
        return;
    }

    if (strcmp(method->valuestring, "claude.approve") == 0) {
        esp_err_t err;
        BaseType_t task_ok;

        /* Already have a pending approval — reject new one */
        if (s_approval_req.pending) {
            send_response_error(id->valuestring, "busy", "approval already pending");
            return;
        }

        const cJSON *tool = cJSON_GetObjectItemCaseSensitive(params, "tool_name");
        const cJSON *desc = cJSON_GetObjectItemCaseSensitive(params, "description");
        const cJSON *req_id = cJSON_GetObjectItemCaseSensitive(params, "id");

        if (set_pending_approval(req_id, tool, desc, true) != ESP_OK) {
            send_response_error(id->valuestring, "invalid_request", "approval is missing id");
            return;
        }

        err = focus_home_for_approval();
        if (err != ESP_OK) {
            reset_approval_state();
            send_response_error(id->valuestring, "ui_control_failed", esp_err_to_name(err));
            return;
        }

        strlcpy(s_approval_rpc_id, id->valuestring, sizeof(s_approval_rpc_id));
        task_ok = xTaskCreatePinnedToCore(approval_rpc_wait_task, "approval_wait",
                                          APPROVAL_WAIT_TASK_STACK, NULL, 4, NULL, 1);
        if (task_ok != pdPASS) {
            reset_approval_state();
            send_response_error(id->valuestring, "no_memory", "approval wait task create failed");
            return;
        }

        /* Notify LVGL task to show approval UI */
        publish_ui_event(APP_EVENT_PERMISSION_REQUEST);
        return;
    }

    if (strcmp(method->valuestring, "home.screensaver") == 0) {
        const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(params, "enabled");
        esp_err_t err =
            app_manager_request_home_screensaver(cJSON_IsTrue(enabled), UI_CONTROL_TIMEOUT_MS);

        if (err != ESP_OK) {
            send_response_error(id->valuestring, "ui_control_failed", esp_err_to_name(err));
            return;
        }
        send_response_ok(id->valuestring, cJSON_CreateObject());
        return;
    }

    if (strcmp(method->valuestring, "app.switch") == 0) {
        const cJSON *app = cJSON_GetObjectItemCaseSensitive(params, "app");
        app_id_t app_id = APP_ID_INVALID;
        esp_err_t err;

        if (!cJSON_IsString(app) || app->valuestring == NULL ||
            !parse_app_id_value(app->valuestring, &app_id)) {
            send_response_error(id->valuestring, "invalid_request",
                                "app must be one of home/trading/slot/settings");
            return;
        }

        err = app_manager_request_switch_to(app_id, UI_CONTROL_TIMEOUT_MS);
        if (err != ESP_OK) {
            send_response_error(id->valuestring, "ui_control_failed", esp_err_to_name(err));
            return;
        }

        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "app", app_id_to_string(app_id));
        send_response_ok(id->valuestring, result);
        return;
    }

    if (strcmp(method->valuestring, "screen.capture.start") == 0) {
        app_screenshot_t capture = {0};
        uint8_t *buffer = NULL;
        char capture_id[32];
        cJSON *result = NULL;
        esp_err_t err;

        buffer = heap_caps_malloc(SCREENSHOT_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buffer == NULL) {
            buffer = heap_caps_malloc(SCREENSHOT_BUFFER_BYTES, MALLOC_CAP_8BIT);
        }
        if (buffer == NULL) {
            send_response_error(id->valuestring, "no_memory",
                                "failed to allocate screenshot buffer");
            return;
        }

        capture.buffer = buffer;
        capture.capacity_bytes = SCREENSHOT_BUFFER_BYTES;
        err = app_manager_request_screenshot(&capture, SCREENSHOT_TIMEOUT_MS);
        if (err != ESP_OK) {
            heap_caps_free(buffer);
            send_response_error(id->valuestring, "capture_failed", esp_err_to_name(err));
            return;
        }
        if (capture.data_size == 0 || capture.info.width == 0 || capture.info.height == 0 ||
            capture.info.stride_bytes == 0 || capture.info.format != APP_SCREENSHOT_FORMAT_RGB565) {
            heap_caps_free(buffer);
            send_response_error(id->valuestring, "capture_failed", "invalid screenshot metadata");
            return;
        }

        s_screenshot_seq++;
        snprintf(capture_id, sizeof(capture_id), "capture-%lu", (unsigned long)s_screenshot_seq);
        result = build_screenshot_start_result(capture_id, &capture);
        if (result == NULL) {
            heap_caps_free(buffer);
            send_response_error(id->valuestring, "no_memory",
                                "failed to build screenshot metadata");
            return;
        }

        send_response_ok(id->valuestring, result);
        err = stream_screenshot_capture(capture_id, &capture);
        if (err != ESP_OK) {
            send_screenshot_error_event(capture_id, esp_err_to_name(err));
        }
        heap_caps_free(buffer);
        return;
    }

    if (strcmp(method->valuestring, "debug.market_snapshot") == 0) {
        cJSON *result = build_market_snapshot_debug();

        if (result == NULL) {
            send_response_error(id->valuestring, "internal_error",
                                "failed to build market snapshot");
            return;
        }
        send_response_ok(id->valuestring, result);
        return;
    }

    if (strcmp(method->valuestring, "slot.export_hit") == 0) {
        slot_hit_export_t hit = {0};
        esp_err_t err = bitcoin_service_read_hit_record(&hit);

        if (err != ESP_OK) {
            send_response_error(id->valuestring, "no_hit_record",
                                err == ESP_ERR_NVS_NOT_FOUND ? "no hit record found"
                                                             : esp_err_to_name(err));
            return;
        }

        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "wif", hit.wif);
        cJSON_AddStringToObject(result, "label", bitcoin_service_label_for_id(hit.label_id));
        cJSON_AddBoolToObject(result, "is_self_test", hit.is_self_test);
        cJSON_AddNumberToObject(result, "created_at", (double)hit.created_at_epoch_s);

        {
            char hash160_hex[41] = {0};
            size_t k;
            for (k = 0; k < sizeof(hit.hash160); k++) {
                snprintf(hash160_hex + k * 2, 3, "%02x", hit.hash160[k]);
            }
            cJSON_AddStringToObject(result, "hash160", hash160_hex);
        }

        mbedtls_platform_zeroize(&hit, sizeof(hit));
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
        ESP_LOGW(TAG, "proto: JSON parse failed");
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

    ESP_LOGI(TAG, "reader_task started");

    for (;;) {
        /* Bypass VFS — read directly from driver ring buffer.
         * The USB Serial JTAG VFS uses a global singleton context (s_ctx)
         * shared between /dev/usbserjtag and /dev/secondary (console).
         * Reading through VFS read() is unreliable when the secondary
         * console is enabled. Direct driver reads work correctly. */
        int bytes_read = usb_serial_jtag_read_bytes(buffer, sizeof(buffer), pdMS_TO_TICKS(20));

        if (bytes_read <= 0) {
            continue;
        }

        for (int i = 0; i < bytes_read; ++i) {
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
    usb_serial_jtag_driver_config_t usb_config = {
        .tx_buffer_size = 256,
        .rx_buffer_size = 1024,
    };

    if (s_started) {
        return ESP_OK;
    }

    s_stdout_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_stdout_lock != NULL, ESP_ERR_NO_MEM, TAG, "failed to create stdout lock");
    s_approval_sem = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_approval_sem != NULL, ESP_ERR_NO_MEM, TAG,
                        "failed to create approval sem");
    reset_approval_state();
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

bool device_link_get_pending_approval(approval_request_t *out)
{
    if (!s_approval_req.pending) {
        return false;
    }
    if (out) {
        *out = s_approval_req;
    }
    return true;
}

void device_link_resolve_approval(approval_decision_t decision)
{
    if (!s_approval_req.pending) {
        return;
    }
    if (!s_approval_uses_rpc) {
        const char *decision_str = "deny";
        cJSON *payload = cJSON_CreateObject();

        if (decision == APPROVAL_DECISION_ALLOW) {
            decision_str = "allow";
        } else if (decision == APPROVAL_DECISION_YOLO) {
            decision_str = "yolo";
        }

        if (payload != NULL) {
            cJSON_AddStringToObject(payload, "id", s_approval_req.id);
            cJSON_AddStringToObject(payload, "decision", decision_str);
            send_event_frame("claude.approval.resolved", payload);
        }
        device_link_dismiss_approval();
        return;
    }
    s_approval_decision = decision;
    xSemaphoreGive(s_approval_sem);
}

void device_link_cancel_approval(void)
{
    if (s_approval_req.pending) {
        if (!s_approval_uses_rpc) {
            reset_approval_state();
            return;
        }
        s_approval_decision = APPROVAL_DECISION_DENY;
        xSemaphoreGive(s_approval_sem);
    }
}

void device_link_dismiss_approval(void)
{
    if (!s_approval_req.pending) {
        return;
    }
    reset_approval_state();
}
