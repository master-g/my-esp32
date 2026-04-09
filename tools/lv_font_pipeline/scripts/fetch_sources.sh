#!/bin/sh

set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
PIPELINE_DIR="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"

# shellcheck source=/dev/null
. "$PIPELINE_DIR/configs/home_status_font.conf"

mkdir -p "$PIPELINE_DIR/$LOCAL_CACHE_DIR"

if [ ! -f "$PIPELINE_DIR/$UPSTREAM_FONT_SOURCE" ]; then
  echo "$UPSTREAM_SOURCE_LABEL font file not found at $UPSTREAM_FONT_SOURCE" >&2
  echo "Run 'npm install' in tools/lv_font_pipeline first." >&2
  exit 1
fi

if [ ! -f "$PIPELINE_DIR/$UPSTREAM_METADATA_SOURCE" ]; then
  echo "$UPSTREAM_SOURCE_LABEL metadata not found at $UPSTREAM_METADATA_SOURCE" >&2
  echo "Run 'npm install' in tools/lv_font_pipeline first." >&2
  exit 1
fi

cp "$PIPELINE_DIR/$UPSTREAM_FONT_SOURCE" "$PIPELINE_DIR/$LOCAL_FONT_FILE"
cp "$PIPELINE_DIR/$UPSTREAM_METADATA_SOURCE" "$PIPELINE_DIR/$LOCAL_METADATA_FILE"

echo "Done."
