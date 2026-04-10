#!/bin/sh

set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
PIPELINE_DIR="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
CONFIG_FILE="${FONT_CONFIG:-$PIPELINE_DIR/configs/departure_mono_11.conf}"

# shellcheck source=/dev/null
. "$CONFIG_FILE"

if [ ! -x "$PIPELINE_DIR/node_modules/.bin/lv_font_conv" ]; then
  echo "lv_font_conv is not installed. Run 'npm install' in tools/lv_font_pipeline first." >&2
  exit 1
fi

if [ ! -f "$PIPELINE_DIR/$LOCAL_FONT_FILE" ]; then
  FONT_CONFIG="$CONFIG_FILE" "$SCRIPT_DIR/fetch_departure_mono.sh"
fi

mkdir -p "$(dirname "$PIPELINE_DIR/$OUTPUT_C")"
mkdir -p "$(dirname "$PIPELINE_DIR/$OUTPUT_H")"

"$PIPELINE_DIR/node_modules/.bin/lv_font_conv" \
  --font "$PIPELINE_DIR/$LOCAL_FONT_FILE" \
  -r "$GLYPH_RANGES" \
  --size "$LV_FONT_SIZE" \
  --bpp "$LV_FONT_BPP" \
  --no-compress \
  --format lvgl \
  --lv-include "$LV_INCLUDE_HEADER" \
  --lv-font-name "$FONT_SYMBOL_NAME" \
  -o "$PIPELINE_DIR/$OUTPUT_C"

node - "$PIPELINE_DIR/$OUTPUT_C" <<'NODE'
const fs = require("fs");

const outputPath = process.argv[2];
let source = fs.readFileSync(outputPath, "utf8");

const licenseBanner = `/*
 * Derived from Departure Mono v1.500 by Helena Zhang.
 * Source: https://github.com/rektdeckard/departure-mono/releases/tag/v1.500
 * License: SIL Open Font License 1.1.
 * See third_party/fonts/departure-mono/OFL-1.1.txt and
 * src/components/ui_fonts/src/generated/README.md.
 */

`;

source = source.replace(".bitmap_format = 1,", ".bitmap_format = 0,");
if (!source.includes(".stride =")) {
  source = source.replace(
    ".bitmap_format = 0,",
    ".bitmap_format = 0,\n    .stride = 0,"
  );
}

if (!source.startsWith("/*\n * Derived from Departure Mono v1.500")) {
  source = licenseBanner + source;
}

fs.writeFileSync(outputPath, source);
NODE

cat > "$PIPELINE_DIR/$OUTPUT_H" <<EOF
#ifndef $HEADER_GUARD
#define $HEADER_GUARD

#include "lvgl.h"

LV_FONT_DECLARE($FONT_SYMBOL_NAME);

#endif
EOF

echo "Generated:"
echo "  $OUTPUT_C"
echo "  $OUTPUT_H"
