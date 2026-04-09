#!/bin/sh

BIN="${ESP32DASH_BIN:-esp32dash}"

if ! command -v "$BIN" >/dev/null 2>&1; then
  exit 0
fi

# Read stdin into a variable so we can inspect it
INPUT=$(cat)

# Check if this is a PermissionRequest event — route to blocking approve handler
EVENT_NAME=$(printf '%s' "$INPUT" | grep -o '"hook_event_name"[[:space:]]*:[[:space:]]*"[^"]*"' | head -1 | sed 's/.*"hook_event_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/')

if [ "$EVENT_NAME" = "PermissionRequest" ]; then
  printf '%s' "$INPUT" | exec "$BIN" claude approve --event-from-stdin
else
  printf '%s' "$INPUT" | "$BIN" claude ingest --event-from-stdin
  exit 0
fi
