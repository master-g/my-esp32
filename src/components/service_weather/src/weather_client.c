#include "weather_client.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "weather_client";

#define WEATHER_CLIENT_URL_LEN 256
#define WEATHER_CLIENT_RESPONSE_CAPACITY 2048

typedef struct {
    char *buffer;
    size_t used;
} weather_http_buffer_t;

static esp_err_t weather_http_event_handler(esp_http_client_event_t *event)
{
    weather_http_buffer_t *payload = (weather_http_buffer_t *)event->user_data;

    if (event == NULL || payload == NULL) {
        return ESP_OK;
    }

    if (event->event_id == HTTP_EVENT_ON_DATA && event->data != NULL && event->data_len > 0) {
        size_t available = WEATHER_CLIENT_RESPONSE_CAPACITY - payload->used - 1;
        size_t copy_len =
            (available < (size_t)event->data_len) ? available : (size_t)event->data_len;

        if (copy_len > 0) {
            memcpy(payload->buffer + payload->used, event->data, copy_len);
            payload->used += copy_len;
            payload->buffer[payload->used] = '\0';
        }
    }

    return ESP_OK;
}

static const char *find_json_key_value(const char *json, const char *key)
{
    char pattern[48];
    const char *pos = NULL;

    if (json == NULL || key == NULL) {
        return NULL;
    }

    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    pos = strstr(json, pattern);
    if (pos == NULL) {
        return NULL;
    }

    return pos + strlen(pattern);
}

static const char *find_current_object(const char *json)
{
    const char *pos = strstr(json, "\"current\":{");

    if (pos == NULL) {
        return NULL;
    }

    return pos;
}

static esp_err_t parse_json_number(const char *json, const char *key, double *value)
{
    const char *cursor = find_json_key_value(json, key);
    char *end = NULL;

    ESP_RETURN_ON_FALSE(cursor != NULL, ESP_ERR_NOT_FOUND, TAG, "key not found: %s", key);
    *value = strtod(cursor, &end);
    ESP_RETURN_ON_FALSE(end != cursor, ESP_ERR_INVALID_RESPONSE, TAG, "invalid number for key: %s",
                        key);
    return ESP_OK;
}

static esp_err_t parse_json_int(const char *json, const char *key, int *value)
{
    double parsed = 0;
    esp_err_t err = parse_json_number(json, key, &parsed);

    ESP_RETURN_ON_ERROR(err, TAG, "failed parsing int key: %s", key);
    *value = (int)parsed;
    return ESP_OK;
}

esp_err_t weather_client_fetch_current(const weather_location_config_t *location,
                                       weather_client_result_t *result)
{
    char url[WEATHER_CLIENT_URL_LEN];
    weather_http_buffer_t payload = {0};
    esp_http_client_config_t config = {0};
    esp_http_client_handle_t client = NULL;
    esp_err_t err = ESP_OK;
    double temperature_c = 0;
    int weather_code = 0;
    int is_day = 0;
    int status = 0;
    const char *current_object = NULL;

    ESP_RETURN_ON_FALSE(result != NULL, ESP_ERR_INVALID_ARG, TAG, "result is required");
    ESP_RETURN_ON_FALSE(location != NULL, ESP_ERR_INVALID_ARG, TAG, "location is required");
    ESP_RETURN_ON_FALSE(location->latitude[0] != '\0', ESP_ERR_INVALID_ARG, TAG,
                        "weather latitude is required");
    ESP_RETURN_ON_FALSE(location->longitude[0] != '\0', ESP_ERR_INVALID_ARG, TAG,
                        "weather longitude is required");

    payload.buffer = calloc(1, WEATHER_CLIENT_RESPONSE_CAPACITY);
    ESP_RETURN_ON_FALSE(payload.buffer != NULL, ESP_ERR_NO_MEM, TAG,
                        "weather payload alloc failed");

    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/"
             "forecast?latitude=%s&longitude=%s&current=temperature_2m,weather_code,is_day&"
             "timezone=auto",
             location->latitude, location->longitude);

    config.url = url;
    config.method = HTTP_METHOD_GET;
    config.event_handler = weather_http_event_handler;
    config.user_data = &payload;
    config.timeout_ms = 10000;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    client = esp_http_client_init(&config);
    ESP_RETURN_ON_FALSE(client != NULL, ESP_ERR_NO_MEM, TAG, "http client init failed");

    err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        free(payload.buffer);
        return err;
    }

    status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (status != 200) {
        free(payload.buffer);
        ESP_RETURN_ON_FALSE(status == 200, ESP_ERR_INVALID_RESPONSE, TAG,
                            "unexpected weather status: %d", status);
    }

    current_object = find_current_object(payload.buffer);
    ESP_RETURN_ON_FALSE(current_object != NULL, ESP_ERR_INVALID_RESPONSE, TAG,
                        "current object not found");

    err = parse_json_number(current_object, "temperature_2m", &temperature_c);
    if (err == ESP_OK) {
        err = parse_json_int(current_object, "weather_code", &weather_code);
    }
    if (err == ESP_OK) {
        err = parse_json_int(current_object, "is_day", &is_day);
    }
    if (err != ESP_OK) {
        free(payload.buffer);
        return err;
    }

    result->temperature_c_tenths = (int16_t)(temperature_c * 10.0);
    result->weather_code = (uint16_t)weather_code;
    result->is_day = (is_day != 0);
    free(payload.buffer);
    return ESP_OK;
}
