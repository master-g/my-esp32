# LV Font Pipeline

This directory contains the local toolchain for generating LVGL-ready icon fonts for the dashboard UI.

What is tracked here:

- the Node dependency manifest and lockfile
- shell scripts for fetching upstream font sources and generating LVGL output
- the checked-in configuration for the Home icon font

What is intentionally not tracked here:

- downloaded upstream font files
- local caches
- `node_modules`
- temporary debug dumps or intermediate font artifacts

The first target is the Home icon font, which is used by the Home page status bar and weather summary.
The second tracked target is the Home page CJK fallback font for bubble text and approval descriptions.

## Current source policy

The pipeline now defaults to `bootstrap-icons` as the packaged upstream dependency, because it ships the Claude/Anthropic brand glyphs and a broad enough utility/weather icon set for the Home page.

The checked-in config now builds a single Home icon font that includes:

- Claude brand glyph
- Wi-Fi glyph
- the weather glyph set used by `weather_mapper`

The generation script resolves every configured icon name from the cached upstream metadata and keeps the order from `ICON_SPECS`. The checked-in config copies the package `WOFF` instead of `WOFF2`, because the current `lv_font_conv` build in this repo does not accept `WOFF2`.

For CJK text we keep a tracked `noto_sans_cjk_12` output under `src/apps/app_home/src/generated/`.
That font is derived from Noto Sans SC / Noto Sans CJK SC. The upstream font binary is not vendored
in this repository; regeneration expects either:

- `NOTO_SANS_SC_SOURCE_FONT=/abs/path/to/NotoSansSC-Regular.otf`, or
- a copied cache file at `.cache/upstream/NotoSansSC-Regular.otf`

The glyph list is extracted from the tracked `noto_sans_cjk_12.c` comment block so host-side text
sanitization can stay aligned with the device font coverage.

## Upstream sources

- `lv_font_conv`: https://github.com/lvgl/lv_font_conv
- `bootstrap-icons`: https://www.npmjs.com/package/bootstrap-icons
- Bootstrap Icons catalog: https://icons.getbootstrap.com/
- Bootstrap Icons font metadata: `font/bootstrap-icons.json` in the npm package
- `notofonts/noto-cjk`: https://github.com/notofonts/noto-cjk
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

Generate the Home icon font into the tracked source tree:

```bash
npm run generate:home-status
```

Generate the tracked CJK fallback font:

```bash
NOTO_SANS_SC_SOURCE_FONT=/abs/path/to/NotoSansSC-Regular.otf npm run generate:cjk-12
```

Or do both:

```bash
npm run build
```

## Output

The generation script writes the LVGL font files to:

- `src/apps/app_home/src/generated/app_home_status_font.c`
- `src/apps/app_home/src/generated/app_home_status_font.h`
- `src/apps/app_home/src/generated/noto_sans_cjk_12.c`
- `src/apps/app_home/src/generated/noto_sans_cjk_12.h`

These generated files are meant to be tracked in git. The source webfont and all process artifacts stay ignored under this tool directory.
