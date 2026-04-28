#ifndef SERVICE_CLAUDE_H
#define SERVICE_CLAUDE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define CLAUDE_STR_WORKSPACE_MAX 24
#define CLAUDE_STR_TITLE_MAX 40
#define CLAUDE_STR_DETAIL_MAX 96
#define CLAUDE_STR_SESSION_MAX 40
#define CLAUDE_STR_EMOTION_MAX 16

typedef enum {
    CLAUDE_CONN_DISCONNECTED = 0,
    CLAUDE_CONN_CONNECTING,
    CLAUDE_CONN_CONNECTED,
    CLAUDE_CONN_AUTH_FAILED,
} claude_conn_state_t;

typedef enum {
    CLAUDE_RUN_UNKNOWN = 0,
    CLAUDE_RUN_WAITING_FOR_INPUT,
    CLAUDE_RUN_PROCESSING,
    CLAUDE_RUN_RUNNING_TOOL,
    CLAUDE_RUN_COMPACTING,
    CLAUDE_RUN_ENDED,
} claude_run_state_t;

typedef struct {
    uint32_t seq;
    claude_conn_state_t conn_state;
    claude_run_state_t run_state;
    bool unread;
    bool stale;
    uint32_t updated_at_epoch_s;
    char session_id[CLAUDE_STR_SESSION_MAX];
    char workspace[CLAUDE_STR_WORKSPACE_MAX];
    char title[CLAUDE_STR_TITLE_MAX];
    char detail[CLAUDE_STR_DETAIL_MAX];
    char emotion[CLAUDE_STR_EMOTION_MAX];
    char event[24];
    char permission_mode[16];
    bool has_pending_prompt;
} claude_snapshot_t;

esp_err_t claude_service_init(void);
esp_err_t claude_service_start(void);
void claude_service_stop(void);
void claude_service_apply_remote_snapshot(const claude_snapshot_t *snapshot);
void claude_service_note_transport_alive(void);
void claude_service_get_snapshot(claude_snapshot_t *out);
bool claude_service_has_unread(void);
void claude_service_mark_read(uint32_t seq);
claude_conn_state_t claude_service_get_conn_state(void);
void claude_service_set_pending_prompt(bool pending);
bool claude_service_get_pending_prompt(void);

/**
 * Check if the bridge has gone stale (no snapshot or heartbeat for a while).
 * Call this from a periodic tick. Returns true if state transitioned
 * from CONNECTED to DISCONNECTED.
 */
bool claude_service_check_staleness(void);

#endif
