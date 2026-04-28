#include "service_claude.h"

#include <stdio.h>
#include <string.h>

#include "core_types/app_event.h"
#include "esp_timer.h"
#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define CLAUDE_STALENESS_TIMEOUT_US (120 * 1000000ULL)

static claude_snapshot_t s_snapshot;
static SemaphoreHandle_t s_mutex;
static int64_t s_last_transport_us;

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
    s_last_transport_us = esp_timer_get_time();
    xSemaphoreGive(s_mutex);
    publish_claude_event();
}

void claude_service_note_transport_alive(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_snapshot.updated_at_epoch_s != 0) {
        s_snapshot.conn_state = CLAUDE_CONN_CONNECTED;
    }
    s_snapshot.stale = false;
    s_last_transport_us = esp_timer_get_time();
    xSemaphoreGive(s_mutex);
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

void claude_service_set_pending_prompt(bool pending)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_snapshot.has_pending_prompt = pending;
    xSemaphoreGive(s_mutex);
    publish_claude_event();
}

bool claude_service_get_pending_prompt(void)
{
    bool pending;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    pending = s_snapshot.has_pending_prompt;
    xSemaphoreGive(s_mutex);
    return pending;
}

bool claude_service_check_staleness(void)
{
    bool became_stale = false;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_snapshot.conn_state == CLAUDE_CONN_CONNECTED && s_last_transport_us > 0) {
        int64_t elapsed = esp_timer_get_time() - s_last_transport_us;
        if (elapsed > (int64_t)CLAUDE_STALENESS_TIMEOUT_US) {
            s_snapshot.conn_state = CLAUDE_CONN_DISCONNECTED;
            s_snapshot.stale = true;
            became_stale = true;
        }
    }
    xSemaphoreGive(s_mutex);

    if (became_stale) {
        publish_claude_event();
    }
    return became_stale;
}
