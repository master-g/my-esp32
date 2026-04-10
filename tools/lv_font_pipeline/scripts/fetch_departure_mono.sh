#!/bin/sh

set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
PIPELINE_DIR="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
CONFIG_FILE="${FONT_CONFIG:-$PIPELINE_DIR/configs/departure_mono_11.conf}"

# shellcheck source=/dev/null
. "$CONFIG_FILE"

mkdir -p "$PIPELINE_DIR/$LOCAL_CACHE_DIR"

if [ ! -f "$PIPELINE_DIR/$LOCAL_ZIP_FILE" ]; then
  curl -A "Codex" -L -o "$PIPELINE_DIR/$LOCAL_ZIP_FILE" "$DEPARTURE_MONO_ZIP_URL"
fi

if [ ! -f "$PIPELINE_DIR/$LOCAL_FONT_FILE" ]; then
  unzip -p "$PIPELINE_DIR/$LOCAL_ZIP_FILE" "$DEPARTURE_MONO_ZIP_OTF_PATH" > \
    "$PIPELINE_DIR/$LOCAL_FONT_FILE"
fi

echo "Done."
