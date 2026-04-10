#!/bin/sh

set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
PIPELINE_DIR="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"

# shellcheck source=/dev/null
. "$PIPELINE_DIR/configs/noto_sans_cjk_12.conf"

if [ ! -x "$PIPELINE_DIR/node_modules/.bin/lv_font_conv" ]; then
  echo "lv_font_conv is not installed. Run 'npm install' in tools/lv_font_pipeline first." >&2
  exit 1
fi

SOURCE_FONT_FILE="${NOTO_SANS_SC_SOURCE_FONT:-$PIPELINE_DIR/$LOCAL_FONT_FILE}"
if [ ! -f "$SOURCE_FONT_FILE" ]; then
  echo "$UPSTREAM_SOURCE_LABEL source font not found at $SOURCE_FONT_FILE" >&2
  echo "Set NOTO_SANS_SC_SOURCE_FONT or copy the upstream OTF into $LOCAL_FONT_FILE" >&2
  exit 1
fi

SYMBOL_SOURCE_FILE="$PIPELINE_DIR/$SYMBOL_SOURCE_C"
if [ ! -f "$SYMBOL_SOURCE_FILE" ]; then
  echo "symbol source file not found at $SYMBOL_SOURCE_FILE" >&2
  exit 1
fi

SYMBOLS="$(
  sed -n '/--symbols/{n;:a;/--size/q;p;n;ba}' "$SYMBOL_SOURCE_FILE" | \
    sed 's/^ \* //' | tr -d '\n'
)"

if [ -z "$SYMBOLS" ]; then
  echo "failed to extract glyph list from $SYMBOL_SOURCE_FILE" >&2
  exit 1
fi

mkdir -p "$(dirname "$PIPELINE_DIR/$OUTPUT_C")"

"$PIPELINE_DIR/node_modules/.bin/lv_font_conv" \
  --font "$SOURCE_FONT_FILE" \
  --symbols "$SYMBOLS" \
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
 * Derived from Noto Sans SC / Noto Sans CJK SC.
 * Upstream copyright: (c) 2014-2021 Adobe (http://www.adobe.com/).
 * License: SIL Open Font License 1.1.
 * See third_party/fonts/noto-sans-cjk/OFL-1.1.txt and
 * src/apps/app_home/src/generated/README.md.
 */

`;

source = source.replace(".bitmap_format = 1,", ".bitmap_format = 0,");
if (!source.includes(".stride =")) {
  source = source.replace(
    ".bitmap_format = 0,",
    ".bitmap_format = 0,\n    .stride = 0,"
  );
}

if (!source.startsWith("/*\n * Derived from Noto Sans SC / Noto Sans CJK SC.")) {
  source = licenseBanner + source;
}

fs.writeFileSync(outputPath, source);
NODE

cat > "$PIPELINE_DIR/$OUTPUT_H" <<EOF
#ifndef NOTO_SANS_CJK_12_H
#define NOTO_SANS_CJK_12_H

#include "lvgl.h"

LV_FONT_DECLARE($FONT_SYMBOL_NAME);

#endif
EOF

echo "Generated:"
echo "  $OUTPUT_C"
echo "  $OUTPUT_H"
echo "  source font: $SOURCE_FONT_FILE"
