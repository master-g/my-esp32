#ifndef DEVICE_PROTOCOL_H
#define DEVICE_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "service_claude.h"

/* The parts of the host/device serial protocol that need no hardware: assembling
 * lines out of a byte stream, parsing frames, and the list of methods the device
 * speaks. Depends only on cJSON and libc so it also builds on the host, where
 * `make test-host` checks it against the frames in protocol/fixtures. */

#define DEVICE_PROTOCOL_LINE_MAX 768

/* Every method the device declares in its hello frame. Request and event
 * handlers compare against these names, so a method cannot be handled without
 * being listed here. */
#define DP_METHOD_DEVICE_INFO "device.info"
#define DP_METHOD_DEVICE_REBOOT "device.reboot"
#define DP_METHOD_CONFIG_EXPORT "config.export"
#define DP_METHOD_CONFIG_SET_MANY "config.set_many"
#define DP_METHOD_WIFI_SCAN "wifi.scan"
#define DP_METHOD_WIFI_PROFILES_LIST "wifi.profiles.list"
#define DP_METHOD_WIFI_PROFILE_ADD "wifi.profile.add"
#define DP_METHOD_WIFI_PROFILE_REMOVE "wifi.profile.remove"
#define DP_METHOD_CLAUDE_UPDATE "claude.update"
#define DP_METHOD_CLAUDE_HEARTBEAT "claude.heartbeat"
#define DP_METHOD_PROTOCOL_ERROR "protocol.error"
#define DP_METHOD_HOME_SCREENSAVER "home.screensaver"
#define DP_METHOD_APP_SWITCH "app.switch"
#define DP_METHOD_SCREEN_CAPTURE_START "screen.capture.start"
#define DP_METHOD_DEBUG_MARKET_SNAPSHOT "debug.market_snapshot"

extern const char *const device_protocol_methods[];
extern const size_t device_protocol_method_count;

typedef struct {
    char line[DEVICE_PROTOCOL_LINE_MAX];
    size_t len;
    size_t dropped;
    bool discarding;
} device_protocol_line_t;

typedef enum {
    DEVICE_PROTOCOL_LINE_NONE = 0,
    DEVICE_PROTOCOL_LINE_READY,    /* assembler->line holds a complete, non-empty line */
    DEVICE_PROTOCOL_LINE_OVERFLOW, /* an oversized line just ended; *dropped_bytes is its size */
} device_protocol_line_event_t;

/* Feed one received byte. A line longer than DEVICE_PROTOCOL_LINE_MAX - 1 bytes is
 * dropped whole and reported once at its newline, so a runaway sender can neither
 * overflow the buffer nor have its tail parsed as a fresh line. */
device_protocol_line_event_t device_protocol_line_feed(device_protocol_line_t *assembler, char ch,
                                                       size_t *dropped_bytes);

/* Parse a claude.update payload. Returns false when a required field (seq,
 * unread, ts) is missing or mistyped; optional strings default to empty. */
bool device_protocol_parse_claude_update(const cJSON *payload, claude_snapshot_t *snapshot);

#endif
