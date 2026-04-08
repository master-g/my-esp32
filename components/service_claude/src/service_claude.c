#include "service_claude.h"

#include <stdio.h>
#include <string.h>

static claude_snapshot_t s_snapshot;

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
    return ESP_OK;
}

void claude_service_stop(void)
{
    s_snapshot.conn_state = CLAUDE_CONN_DISCONNECTED;
}

const claude_snapshot_t *claude_service_get_snapshot(void)
{
    return &s_snapshot;
}

bool claude_service_has_unread(void)
{
    return s_snapshot.unread;
}

void claude_service_mark_read(uint32_t seq)
{
    if (seq == s_snapshot.seq) {
        s_snapshot.unread = false;
    }
}

claude_conn_state_t claude_service_get_conn_state(void)
{
    return s_snapshot.conn_state;
}
