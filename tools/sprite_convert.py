#!/usr/bin/env python3
"""Convert Notchi sprite sheets to LVGL v9 C arrays (ARGB8888 format)."""

import os
import sys
from PIL import Image

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS_DIR = os.path.join(
    REPO_ROOT,
    "docs/research/notchi/notchi/notchi/Assets.xcassets",
)
OUTPUT_DIR = os.path.join(REPO_ROOT, "src/apps/app_home/src/generated")

FRAME_W = 64
FRAME_H = 64
BYTES_PER_PIXEL = 4  # ARGB8888
FRAME_DATA_SIZE = FRAME_W * FRAME_H * BYTES_PER_PIXEL
STRIDE = FRAME_W * BYTES_PER_PIXEL

SPRITES = [
    ("idle", "idle_neutral", 6),
    ("idle_happy", "idle_happy", 6),
    ("idle_sad", "idle_sad", 6),
    ("idle_sob", "idle_sob", 6),
    ("working", "working_neutral", 6),
    ("working_happy", "working_happy", 6),
    ("working_sad", "working_sad", 6),
    ("waiting", "waiting_neutral", 6),
    ("waiting_happy", "waiting_happy", 6),
    ("waiting_sad", "waiting_sad", 6),
    ("compacting", "compacting_neutral", 5),
    ("sleeping", "sleeping_neutral", 6),
    ("sleeping_happy", "sleeping_happy", 6),
]


def convert_sheet(symbol_name: str, asset_name: str, num_frames: int) -> str:
    """Convert one sprite sheet PNG to LVGL C source."""
    png_path = os.path.join(ASSETS_DIR, f"{asset_name}.imageset", "sprite_sheet.png")
    img = Image.open(png_path).convert("RGBA")

    if img.height != FRAME_H or img.width % FRAME_W != 0:
        raise ValueError(
            f"{asset_name} must be {FRAME_H}px tall and a multiple of {FRAME_W}px wide"
        )

    actual_frames = img.width // FRAME_W
    if actual_frames != num_frames:
        raise ValueError(f"{asset_name} has {actual_frames} frames, expected {num_frames}")

    lines = [
        f'/* Auto-generated from {asset_name} — do not edit */',
        '#include "lvgl.h"',
        "",
    ]

    # Emit raw data for all frames as one contiguous array
    data_var = f"sprite_{symbol_name}_data"
    lines.append(f"static const uint8_t {data_var}[] = {{")

    for fi in range(num_frames):
        frame = img.crop((fi * FRAME_W, 0, (fi + 1) * FRAME_W, FRAME_H))
        pixels = list(frame.getdata())  # [(R,G,B,A), ...]

        lines.append(f"    /* frame {fi} */")
        for r, g, b, a in pixels:
            for byte in (b, g, r, a):
                lines.append(f"    0x{byte:02x},")

    lines.append("};")
    lines.append("")

    # Emit descriptor array
    dsc_var = f"sprite_{symbol_name}_frames"
    lines.append(f"const lv_image_dsc_t {dsc_var}[{num_frames}] = {{")
    for fi in range(num_frames):
        offset = fi * FRAME_DATA_SIZE
        lines.append("    {")
        lines.append("        .header.magic = LV_IMAGE_HEADER_MAGIC,")
        lines.append("        .header.cf = LV_COLOR_FORMAT_ARGB8888,")
        lines.append(f"        .header.w = {FRAME_W},")
        lines.append(f"        .header.h = {FRAME_H},")
        lines.append(f"        .header.stride = {STRIDE},")
        lines.append(f"        .data_size = {FRAME_DATA_SIZE},")
        lines.append(f"        .data = &{data_var}[{offset}],")
        lines.append("    },")
    lines.append("};")
    lines.append("")

    return "\n".join(lines)


def generate_header() -> str:
    """Generate the shared header with extern declarations."""
    lines = [
        "/* Auto-generated — do not edit */",
        "#ifndef SPRITE_FRAMES_H",
        "#define SPRITE_FRAMES_H",
        "",
        '#include "lvgl.h"',
        "",
        f"#define SPRITE_FRAME_W {FRAME_W}",
        f"#define SPRITE_FRAME_H {FRAME_H}",
        "",
    ]

    for symbol_name, _, num_frames in SPRITES:
        lines.append(f"#define SPRITE_{symbol_name.upper()}_FRAME_COUNT {num_frames}")
    lines.append("")

    for symbol_name, _, _ in SPRITES:
        lines.append(
            f"extern const lv_image_dsc_t sprite_{symbol_name}_frames"
            f"[SPRITE_{symbol_name.upper()}_FRAME_COUNT];"
        )

    lines.extend(["", "#endif /* SPRITE_FRAMES_H */", ""])
    return "\n".join(lines)


def main():
    os.makedirs(OUTPUT_DIR, exist_ok=True)

    for symbol_name, asset_name, num_frames in SPRITES:
        c_src = convert_sheet(symbol_name, asset_name, num_frames)
        out_path = os.path.join(OUTPUT_DIR, f"sprite_{symbol_name}.c")
        with open(out_path, "w") as f:
            f.write(c_src)
        size_kb = os.path.getsize(out_path) / 1024
        print(f"  {out_path} ({size_kb:.0f} KB)")

    header = generate_header()
    header_path = os.path.join(OUTPUT_DIR, "sprite_frames.h")
    with open(header_path, "w") as f:
        f.write(header)
    print(f"  {header_path}")

    total_frames = sum(nf for _, _, nf in SPRITES)
    total_bytes = total_frames * FRAME_DATA_SIZE
    print(f"\nTotal: {total_frames} frames, {total_bytes / 1024:.0f} KB raw data in flash")


if __name__ == "__main__":
    main()
