#!/bin/sh

BIN="${ESP32DASH_BIN:-esp32dash}"

if command -v "$BIN" >/dev/null 2>&1; then
  exec "$BIN" claude ingest --event-from-stdin
fi

exit 0
