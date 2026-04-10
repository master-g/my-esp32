# LV Font Pipeline

This directory contains the local toolchain for generating LVGL-ready fonts for the dashboard UI.

What is tracked here:

- the Node dependency manifest and lockfile
- shell scripts for fetching upstream font sources and generating LVGL output
- the checked-in configuration for the Home icon font
- the checked-in configuration for shared Departure Mono text fonts

What is intentionally not tracked here:

- downloaded upstream font files
- local caches
- `node_modules`
- temporary debug dumps or intermediate font artifacts

The first target is the Home icon font, which is used by the Home page status bar and weather summary.
The second tracked target is the shared Departure Mono text font set used across the UI.
The third tracked target is the CJK fallback font set used when UI text contains Chinese.

## Current source policy

The pipeline now defaults to `bootstrap-icons` as the packaged upstream dependency, because it ships the Claude/Anthropic brand glyphs and a broad enough utility/weather icon set for the Home page.

The checked-in config now builds a single Home icon font that includes:

- Claude brand glyph
- Wi-Fi glyph
- the weather glyph set used by `weather_mapper`

The generation script resolves every configured icon name from the cached upstream metadata and keeps the order from `ICON_SPECS`. The checked-in config copies the package `WOFF` instead of `WOFF2`, because the current `lv_font_conv` build in this repo does not accept `WOFF2`.

For UI text we keep tracked Departure Mono outputs under `src/components/ui_fonts/src/generated/`.
They are derived from the official `DepartureMono-Regular.otf` release package and generated at
11px, 22px, and 44px for the dashboard's monospace type scale.

For CJK text we keep tracked Noto fallback outputs under `src/components/ui_fonts/src/generated/`.
These fonts are derived from Noto Sans SC / Noto Sans CJK SC. The upstream font binary is not
vendored in this repository; regeneration expects either:

- `NOTO_SANS_SC_SOURCE_FONT=/abs/path/to/NotoSansSC-Regular.otf`, or
- a copied cache file at `.cache/upstream/NotoSansSC-Regular.otf`

The glyph list is extracted from the tracked `noto_sans_cjk_12.c` comment block so host-side text
sanitization can stay aligned with the device font coverage. The shared 11px and 22px fallback
fonts reuse the same symbol list.

## Upstream sources

- `lv_font_conv`: https://github.com/lvgl/lv_font_conv
- `bootstrap-icons`: https://www.npmjs.com/package/bootstrap-icons
- Bootstrap Icons catalog: https://icons.getbootstrap.com/
- Bootstrap Icons font metadata: `font/bootstrap-icons.json` in the npm package
- `Departure Mono`: https://departuremono.com/
- `Departure Mono` releases: https://github.com/rektdeckard/departure-mono/releases
- `notofonts/noto-cjk`: https://github.com/notofonts/noto-cjk
- Departure Mono license copy: `third_party/fonts/departure-mono/OFL-1.1.txt`
- Noto Sans SC license copy: `third_party/fonts/noto-sans-cjk/OFL-1.1.txt`

The configured icon unicodes are resolved from the cached metadata file during generation. JSON flat maps such as `bootstrap-icons.json` and Font Awesome style YAML metadata are both supported.

The script also forces `--no-compress` and normalizes the generated `font_dsc` initializer for LVGL 9. The bundled `lv_font_conv` version still emits an LVGL 8-style initializer for plain bitmap fonts, which would otherwise leave `bitmap_format` and `stride` incompatible with this firmware tree.

## Usage

Install tooling once:

```bash
cd tools/lv_font_pipeline
npm install
```

Copy the required upstream webfont and metadata into the local cache:

```bash
npm run fetch:sources
```

Fetch and cache Departure Mono:

```bash
npm run fetch:departure-mono
```

Generate the Home icon font into the tracked source tree:

```bash
npm run generate:home-status
```

Generate the tracked Departure Mono text fonts:

```bash
npm run generate:departure
```

Generate the tracked CJK fallback font:

```bash
NOTO_SANS_SC_SOURCE_FONT=/abs/path/to/NotoSansSC-Regular.otf npm run generate:cjk-12
```

Generate the shared CJK fallback fonts:

```bash
NOTO_SANS_SC_SOURCE_FONT=/abs/path/to/NotoSansSC-Regular.otf npm run generate:cjk-11
NOTO_SANS_SC_SOURCE_FONT=/abs/path/to/NotoSansSC-Regular.otf npm run generate:cjk-22
```

Build the tracked assets that do not require the external Noto source:

```bash
npm run build
```

## Output

The generation script writes the LVGL font files to:

- `src/apps/app_home/src/generated/app_home_status_font.c`
- `src/apps/app_home/src/generated/app_home_status_font.h`
- `src/components/ui_fonts/src/generated/departure_mono_11.c`
- `src/components/ui_fonts/include/generated/departure_mono_11.h`
- `src/components/ui_fonts/src/generated/departure_mono_22.c`
- `src/components/ui_fonts/include/generated/departure_mono_22.h`
- `src/components/ui_fonts/src/generated/departure_mono_44.c`
- `src/components/ui_fonts/include/generated/departure_mono_44.h`
- `src/components/ui_fonts/src/generated/noto_sans_cjk_11.c`
- `src/components/ui_fonts/include/generated/noto_sans_cjk_11.h`
- `src/components/ui_fonts/src/generated/noto_sans_cjk_22.c`
- `src/components/ui_fonts/include/generated/noto_sans_cjk_22.h`
- `src/apps/app_home/src/generated/noto_sans_cjk_12.c`
- `src/apps/app_home/src/generated/noto_sans_cjk_12.h`

These generated files are meant to be tracked in git. The source webfont and all process artifacts stay ignored under this tool directory.
