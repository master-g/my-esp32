#include "service_claude.h"

#include <stdio.h>
#include <string.h>

#include "core_types/app_event.h"
#include "event_bus.h"

static claude_snapshot_t s_snapshot;

static void publish_claude_event(void)
{
    app_event_t event = {
        .type = APP_EVENT_DATA_CLAUDE,
        .payload = &s_snapshot,
    };

    event_bus_publish(&event);
}

esp_err_t claude_service_init(void)
{
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.conn_state = CLAUDE_CONN_DISCONNECTED;
    s_snapshot.run_state = CLAUDE_RUN_UNKNOWN;
    snprintf(s_snapshot.title, sizeof(s_snapshot.title), "%s", "Claude bridge offline");
    return ESP_OK;
}

esp_err_t claude_service_start(void)
{
    s_snapshot.conn_state = CLAUDE_CONN_CONNECTING;
    publish_claude_event();
    return ESP_OK;
}

void claude_service_stop(void)
{
    s_snapshot.conn_state = CLAUDE_CONN_DISCONNECTED;
    publish_claude_event();
}

void claude_service_apply_remote_snapshot(const claude_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    memcpy(&s_snapshot, snapshot, sizeof(s_snapshot));
    s_snapshot.conn_state = CLAUDE_CONN_CONNECTED;
    s_snapshot.stale = false;
    publish_claude_event();
}

const claude_snapshot_t *claude_service_get_snapshot(void) { return &s_snapshot; }

bool claude_service_has_unread(void) { return s_snapshot.unread; }

void claude_service_mark_read(uint32_t seq)
{
    if (seq == s_snapshot.seq) {
        s_snapshot.unread = false;
        publish_claude_event();
    }
}

claude_conn_state_t claude_service_get_conn_state(void) { return s_snapshot.conn_state; }
