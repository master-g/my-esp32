#include "weather_client.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "weather_client";

#define WEATHER_CLIENT_URL_LEN 256
#define WEATHER_CLIENT_RESPONSE_CAPACITY 4096

typedef struct {
    char *buffer;
    size_t used;
    bool overflow;
} weather_http_buffer_t;

static esp_err_t weather_http_event_handler(esp_http_client_event_t *event)
{
    weather_http_buffer_t *payload = (weather_http_buffer_t *)event->user_data;

    if (event == NULL || payload == NULL) {
        return ESP_OK;
    }

    if (event->event_id == HTTP_EVENT_ON_DATA && event->data != NULL && event->data_len > 0) {
        if (payload->used + (size_t)event->data_len >= WEATHER_CLIENT_RESPONSE_CAPACITY) {
            payload->overflow = true;
            return ESP_OK;
        }

        memcpy(payload->buffer + payload->used, event->data, event->data_len);
        payload->used += (size_t)event->data_len;
        payload->buffer[payload->used] = '\0';
    }

    return ESP_OK;
}

static esp_err_t parse_current_weather(const char *json_str, weather_client_result_t *result)
{
    cJSON *root = cJSON_Parse(json_str);
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_INVALID_RESPONSE, TAG, "JSON parse failed");

    cJSON *current = cJSON_GetObjectItem(root, "current");
    if (current == NULL || !cJSON_IsObject(current)) {
        cJSON_Delete(root);
        ESP_RETURN_ON_FALSE(false, ESP_ERR_INVALID_RESPONSE, TAG, "\"current\" object missing");
    }

    cJSON *temp = cJSON_GetObjectItem(current, "temperature_2m");
    cJSON *code = cJSON_GetObjectItem(current, "weather_code");
    cJSON *day = cJSON_GetObjectItem(current, "is_day");

    if (!cJSON_IsNumber(temp) || !cJSON_IsNumber(code) || !cJSON_IsNumber(day)) {
        cJSON_Delete(root);
        ESP_RETURN_ON_FALSE(false, ESP_ERR_INVALID_RESPONSE, TAG,
                            "missing or non-numeric fields in current");
    }

    result->temperature_c_tenths = (int16_t)(cJSON_GetNumberValue(temp) * 10.0);
    result->weather_code = (uint16_t)cJSON_GetNumberValue(code);
    result->is_day = ((int)cJSON_GetNumberValue(day) != 0);

    cJSON_Delete(root);
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
    int status = 0;

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
    if (client == NULL) {
        free(payload.buffer);
        ESP_RETURN_ON_FALSE(false, ESP_ERR_NO_MEM, TAG, "http client init failed");
    }

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
        ESP_RETURN_ON_FALSE(false, ESP_ERR_INVALID_RESPONSE, TAG, "unexpected weather status: %d",
                            status);
    }

    if (payload.overflow) {
        free(payload.buffer);
        ESP_RETURN_ON_FALSE(false, ESP_ERR_INVALID_SIZE, TAG, "response exceeded %d byte limit",
                            WEATHER_CLIENT_RESPONSE_CAPACITY);
    }

    err = parse_current_weather(payload.buffer, result);
    free(payload.buffer);
    return err;
}
