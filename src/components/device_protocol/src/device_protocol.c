#include "device_protocol.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

const char *const device_protocol_methods[] = {
    DP_METHOD_DEVICE_INFO,
    DP_METHOD_DEVICE_REBOOT,
    DP_METHOD_CONFIG_EXPORT,
    DP_METHOD_CONFIG_SET_MANY,
    DP_METHOD_WIFI_SCAN,
    DP_METHOD_WIFI_PROFILES_LIST,
    DP_METHOD_WIFI_PROFILE_ADD,
    DP_METHOD_WIFI_PROFILE_REMOVE,
    DP_METHOD_CLAUDE_UPDATE,
    DP_METHOD_CLAUDE_HEARTBEAT,
    DP_METHOD_PROTOCOL_ERROR,
    DP_METHOD_HOME_SCREENSAVER,
    DP_METHOD_APP_SWITCH,
    DP_METHOD_SCREEN_CAPTURE_START,
    DP_METHOD_DEBUG_MARKET_SNAPSHOT,
};
const size_t device_protocol_method_count =
    sizeof(device_protocol_methods) / sizeof(device_protocol_methods[0]);

device_protocol_line_event_t device_protocol_line_feed(device_protocol_line_t *assembler, char ch,
                                                       size_t *dropped_bytes)
{
    if (ch == '\r') {
        return DEVICE_PROTOCOL_LINE_NONE;
    }
    if (ch == '\n') {
        device_protocol_line_event_t event = DEVICE_PROTOCOL_LINE_NONE;

        if (assembler->discarding) {
            if (dropped_bytes != NULL) {
                *dropped_bytes = assembler->dropped;
            }
            event = DEVICE_PROTOCOL_LINE_OVERFLOW;
        } else if (assembler->len > 0) {
            assembler->line[assembler->len] = '\0';
            event = DEVICE_PROTOCOL_LINE_READY;
        }
        assembler->len = 0;
        assembler->dropped = 0;
        assembler->discarding = false;
        return event;
    }

    if (assembler->discarding) {
        assembler->dropped++;
    } else if (assembler->len + 1 < sizeof(assembler->line)) {
        assembler->line[assembler->len++] = ch;
    } else {
        assembler->dropped = assembler->len + 1;
        assembler->discarding = true;
        assembler->len = 0;
    }
    return DEVICE_PROTOCOL_LINE_NONE;
}

static void copy_json_string(const cJSON *object, const char *key, char *dst, size_t dst_size)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);

    if (cJSON_IsString(item) && item->valuestring != NULL) {
        snprintf(dst, dst_size, "%s", item->valuestring);
    }
}

static claude_run_state_t claude_status_from_text(const char *status)
{
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

/* `pending` is null/absent or {"kind": "approval"|"prompt", "type": "<coarse type>"}. */
static void parse_claude_pending(const cJSON *pending, claude_snapshot_t *snapshot)
{
    char kind[16] = {0};

    if (!cJSON_IsObject(pending)) {
        return;
    }

    copy_json_string(pending, "kind", kind, sizeof(kind));
    if (strcmp(kind, "approval") == 0) {
        snapshot->pending_kind = CLAUDE_PENDING_APPROVAL;
    } else if (strcmp(kind, "prompt") == 0) {
        snapshot->pending_kind = CLAUDE_PENDING_PROMPT;
    } else {
        return;
    }
    copy_json_string(pending, "type", snapshot->pending_type, sizeof(snapshot->pending_type));
}

bool device_protocol_parse_claude_update(const cJSON *payload, claude_snapshot_t *snapshot)
{
    const cJSON *seq = cJSON_GetObjectItemCaseSensitive(payload, "seq");
    const cJSON *unread = cJSON_GetObjectItemCaseSensitive(payload, "unread");
    const cJSON *ts = cJSON_GetObjectItemCaseSensitive(payload, "ts");
    char status[32] = {0};

    if (!cJSON_IsObject(payload) || snapshot == NULL || !cJSON_IsNumber(seq) ||
        !cJSON_IsBool(unread) || !cJSON_IsNumber(ts)) {
        return false;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->seq = (uint32_t)seq->valuedouble;
    snapshot->unread = cJSON_IsTrue(unread);
    snapshot->updated_at_epoch_s = (uint32_t)ts->valuedouble;
    copy_json_string(payload, "session_id", snapshot->session_id, sizeof(snapshot->session_id));
    copy_json_string(payload, "workspace", snapshot->workspace, sizeof(snapshot->workspace));
    copy_json_string(payload, "title", snapshot->title, sizeof(snapshot->title));
    copy_json_string(payload, "detail", snapshot->detail, sizeof(snapshot->detail));
    copy_json_string(payload, "emotion", snapshot->emotion, sizeof(snapshot->emotion));
    copy_json_string(payload, "event", snapshot->event, sizeof(snapshot->event));
    copy_json_string(payload, "permission_mode", snapshot->permission_mode,
                     sizeof(snapshot->permission_mode));
    copy_json_string(payload, "status", status, sizeof(status));
    snapshot->run_state = claude_status_from_text(status);
    parse_claude_pending(cJSON_GetObjectItemCaseSensitive(payload, "pending"), snapshot);
    {
        const cJSON *working_secs = cJSON_GetObjectItemCaseSensitive(payload, "working_secs");

        if (cJSON_IsNumber(working_secs) && working_secs->valuedouble >= 0) {
            snapshot->working = true;
            snapshot->working_secs = (uint32_t)working_secs->valuedouble;
        }
    }
    return true;
}
