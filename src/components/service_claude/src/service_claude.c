#include "service_claude.h"

#include <stdio.h>
#include <string.h>

#include "core_types/app_event.h"
#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static claude_snapshot_t s_snapshot;
static SemaphoreHandle_t s_mutex;

static void publish_claude_event(void)
{
    app_event_t event = {
        .type = APP_EVENT_DATA_CLAUDE,
        .payload = NULL,
    };

    event_bus_publish(&event);
}

esp_err_t claude_service_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.conn_state = CLAUDE_CONN_DISCONNECTED;
    s_snapshot.run_state = CLAUDE_RUN_UNKNOWN;
    snprintf(s_snapshot.title, sizeof(s_snapshot.title), "%s", "Claude bridge offline");
    return ESP_OK;
}

esp_err_t claude_service_start(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_snapshot.conn_state = CLAUDE_CONN_CONNECTING;
    xSemaphoreGive(s_mutex);
    publish_claude_event();
    return ESP_OK;
}

void claude_service_stop(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_snapshot.conn_state = CLAUDE_CONN_DISCONNECTED;
    xSemaphoreGive(s_mutex);
    publish_claude_event();
}

void claude_service_apply_remote_snapshot(const claude_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(&s_snapshot, snapshot, sizeof(s_snapshot));
    s_snapshot.conn_state = CLAUDE_CONN_CONNECTED;
    s_snapshot.stale = false;
    xSemaphoreGive(s_mutex);
    publish_claude_event();
}

void claude_service_get_snapshot(claude_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_snapshot;
    xSemaphoreGive(s_mutex);
}

bool claude_service_has_unread(void)
{
    bool unread;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    unread = s_snapshot.unread;
    xSemaphoreGive(s_mutex);
    return unread;
}

void claude_service_mark_read(uint32_t seq)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (seq == s_snapshot.seq) {
        s_snapshot.unread = false;
    }
    xSemaphoreGive(s_mutex);
    publish_claude_event();
}

claude_conn_state_t claude_service_get_conn_state(void)
{
    claude_conn_state_t state;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    state = s_snapshot.conn_state;
    xSemaphoreGive(s_mutex);
    return state;
}
