#!/bin/sh

BIN="${CLAUDE_BRIDGE_BIN:-claude-bridge}"

if command -v "$BIN" >/dev/null 2>&1; then
  exec "$BIN" send --event-from-stdin
fi

exit 0
