#!/bin/sh

set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
PIPELINE_DIR="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"

# shellcheck source=/dev/null
. "$PIPELINE_DIR/configs/home_status_font.conf"

if [ ! -x "$PIPELINE_DIR/node_modules/.bin/lv_font_conv" ]; then
  echo "lv_font_conv is not installed. Run 'npm install' in tools/lv_font_pipeline first." >&2
  exit 1
fi

if [ ! -f "$PIPELINE_DIR/$LOCAL_FONT_FILE" ] || [ ! -f "$PIPELINE_DIR/$LOCAL_METADATA_FILE" ]; then
  "$SCRIPT_DIR/fetch_sources.sh"
fi

SOURCE_FONT_FILE="$PIPELINE_DIR/$LOCAL_FONT_FILE"
RESOLVED_GLYPHS_FILE="$(mktemp "${TMPDIR:-/tmp}/home_icon_glyphs.XXXXXX.json")"
cleanup() {
  rm -f "$RESOLVED_GLYPHS_FILE"
}
trap cleanup EXIT INT TERM HUP

ICON_SPECS="${ICON_SPECS:-}" \
  node - "$PIPELINE_DIR/$LOCAL_METADATA_FILE" "$RESOLVED_GLYPHS_FILE" <<'NODE'
const fs = require("fs");
const path = require("path");
const YAML = require("yaml");

const metadataPath = process.argv[2];
const outputPath = process.argv[3];
const specText = process.env.ICON_SPECS || "";

const raw = fs.readFileSync(metadataPath, "utf8");
const metadata =
  path.extname(metadataPath).toLowerCase() === ".json"
    ? JSON.parse(raw)
    : YAML.parse(raw);
const root = metadata && typeof metadata.icons === "object" ? metadata.icons : metadata;
const specs = specText
  .split(/\r?\n/)
  .map((line) => line.trim())
  .filter(Boolean);

function resolveCodepoint(entry) {
  if (typeof entry === "number" && Number.isInteger(entry)) {
    return entry;
  }
  if (typeof entry === "string" && /^[0-9a-f]+$/i.test(entry)) {
    return parseInt(entry, 16);
  }
  if (entry && typeof entry.unicode === "number" && Number.isInteger(entry.unicode)) {
    return entry.unicode;
  }
  if (entry && typeof entry.unicode === "string" && /^[0-9a-f]+$/i.test(entry.unicode)) {
    return parseInt(entry.unicode, 16);
  }
  return null;
}

function utf8Escape(codepoint) {
  return [...Buffer.from(String.fromCodePoint(codepoint), "utf8")]
    .map((byte) => `\\x${byte.toString(16).toUpperCase().padStart(2, "0")}`)
    .join("");
}

const resolved = specs.map((line) => {
  const parts = line.split("|").map((part) => part.trim()).filter(Boolean);
  const macro = parts.shift();
  const candidates = parts;

  for (const name of candidates) {
    const codepoint = resolveCodepoint(root?.[name]);
    if (codepoint !== null) {
      return {
        macro,
        icon_name: name,
        codepoint,
        codepoint_hex: `0x${codepoint.toString(16).toUpperCase()}`,
        utf8_escape: utf8Escape(codepoint),
      };
    }
  }

  throw new Error(`Could not resolve ${macro} from ${candidates.join(", ") || "<none>"}`);
});

fs.writeFileSync(outputPath, JSON.stringify(resolved, null, 2));
NODE

RANGE_LIST="$(
  node - "$RESOLVED_GLYPHS_FILE" <<'NODE'
const fs = require("fs");
const resolved = JSON.parse(fs.readFileSync(process.argv[2], "utf8"));
const uniqueCodepoints = [...new Set(resolved.map((glyph) => glyph.codepoint_hex))];
process.stdout.write(uniqueCodepoints.join(","));
NODE
)"

mkdir -p "$(dirname "$PIPELINE_DIR/$OUTPUT_C")"

"$PIPELINE_DIR/node_modules/.bin/lv_font_conv" \
  --font "$SOURCE_FONT_FILE" \
  -r "$RANGE_LIST" \
  --size "$LV_FONT_SIZE" \
  --bpp "$LV_FONT_BPP" \
  --no-compress \
  --format lvgl \
  --lv-include "$LV_INCLUDE_HEADER" \
  -o "$PIPELINE_DIR/$OUTPUT_C"

node - "$PIPELINE_DIR/$OUTPUT_C" <<'NODE'
const fs = require("fs");

const outputPath = process.argv[2];
let source = fs.readFileSync(outputPath, "utf8");

/*
 * lv_font_conv 1.5.x still emits an LVGL 8-style font_dsc initializer.
 * We force uncompressed output, then add the explicit stride field LVGL 9 expects.
 */
source = source.replace(".bitmap_format = 1,", ".bitmap_format = 0,");
if (!source.includes(".stride =")) {
  source = source.replace(
    ".bitmap_format = 0,",
    ".bitmap_format = 0,\n    .stride = 0,"
  );
}

fs.writeFileSync(outputPath, source);
NODE

HEADER_DEFINES="$(
  node - "$RESOLVED_GLYPHS_FILE" <<'NODE'
const fs = require("fs");
const resolved = JSON.parse(fs.readFileSync(process.argv[2], "utf8"));
for (const glyph of resolved) {
  console.log(`#define ${glyph.macro} "${glyph.utf8_escape}"`);
}
NODE
)"

cat > "$PIPELINE_DIR/$OUTPUT_H" <<EOF
#ifndef APP_HOME_STATUS_FONT_H
#define APP_HOME_STATUS_FONT_H

#include "lvgl.h"

LV_FONT_DECLARE($FONT_SYMBOL_NAME);

$HEADER_DEFINES

#endif
EOF

PRIMARY_GLYPH_CODEPOINT="$(
  node - "$RESOLVED_GLYPHS_FILE" <<'NODE'
const fs = require("fs");
const resolved = JSON.parse(fs.readFileSync(process.argv[2], "utf8"));
const primary = resolved.find((glyph) => glyph.macro === "APP_HOME_SYMBOL_CLAUDE");
if (primary) {
  process.stdout.write(primary.codepoint_hex);
}
NODE
)"

echo "Generated:"
echo "  $OUTPUT_C"
echo "  $OUTPUT_H"
echo "  glyphs: $RANGE_LIST"
echo "  claude codepoint: $PRIMARY_GLYPH_CODEPOINT"
