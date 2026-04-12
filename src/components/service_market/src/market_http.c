#include "market_http.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"

typedef struct {
    char *buffer;
    size_t capacity;
    size_t used;
    bool overflow;
} market_http_buffer_t;

static esp_err_t market_http_event_handler(esp_http_client_event_t *event)
{
    market_http_buffer_t *payload = (market_http_buffer_t *)event->user_data;

    if (event == NULL || payload == NULL) {
        return ESP_OK;
    }

    if (event->event_id != HTTP_EVENT_ON_DATA || event->data == NULL || event->data_len <= 0) {
        return ESP_OK;
    }

    if ((payload->used + (size_t)event->data_len + 1U) > payload->capacity) {
        payload->overflow = true;
        return ESP_OK;
    }

    memcpy(payload->buffer + payload->used, event->data, (size_t)event->data_len);
    payload->used += (size_t)event->data_len;
    payload->buffer[payload->used] = '\0';
    return ESP_OK;
}

esp_err_t market_http_get_json(const char *url, size_t capacity, char **out_body)
{
    esp_http_client_config_t config = {0};
    esp_http_client_handle_t client = NULL;
    market_http_buffer_t payload = {0};
    esp_err_t err;
    int status = 0;

    ESP_RETURN_ON_FALSE(url != NULL, ESP_ERR_INVALID_ARG, "market_http", "url is required");
    ESP_RETURN_ON_FALSE(out_body != NULL, ESP_ERR_INVALID_ARG, "market_http",
                        "out body is required");
    ESP_RETURN_ON_FALSE(capacity > 0, ESP_ERR_INVALID_ARG, "market_http",
                        "capacity must be positive");

    *out_body = NULL;
    payload.buffer = calloc(1, capacity);
    ESP_RETURN_ON_FALSE(payload.buffer != NULL, ESP_ERR_NO_MEM, "market_http",
                        "response buffer alloc failed");
    payload.capacity = capacity;

    config.url = url;
    config.method = HTTP_METHOD_GET;
    config.event_handler = market_http_event_handler;
    config.user_data = &payload;
    config.timeout_ms = 10000;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    client = esp_http_client_init(&config);
    if (client == NULL) {
        free(payload.buffer);
        ESP_RETURN_ON_FALSE(false, ESP_ERR_NO_MEM, "market_http", "http client init failed");
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
        ESP_RETURN_ON_FALSE(false, ESP_ERR_INVALID_RESPONSE, "market_http",
                            "unexpected HTTP status: %d", status);
    }

    if (payload.overflow) {
        free(payload.buffer);
        ESP_RETURN_ON_FALSE(false, ESP_ERR_INVALID_SIZE, "market_http",
                            "response exceeded %u bytes", (unsigned)capacity);
    }

    *out_body = payload.buffer;
    return ESP_OK;
}
