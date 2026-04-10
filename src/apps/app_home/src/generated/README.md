# Generated Assets For `app_home`

This directory contains tracked generated assets used by the Home page.

`app_home_status_font`
- Source: Bootstrap Icons webfont.
- Generation pipeline: `tools/lv_font_pipeline/scripts/generate_home_status_font.sh`
- License and provenance: `tools/lv_font_pipeline/README.md`

`noto_sans_cjk_12`
- Purpose: 12px / 2bpp fallback font for bubble text and approval descriptions.
- Upstream font family: Noto Sans CJK SC / Noto Sans SC.
- Upstream artifact reference: `notofonts/noto-cjk`, `Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf`
  and `Sans/SubsetOTF/SC/NotoSansSC-Regular.otf`.
- Upstream copyright:
  `(c) 2014-2021 Adobe (http://www.adobe.com/).`
- License: SIL Open Font License 1.1.
  See `third_party/fonts/noto-sans-cjk/OFL-1.1.txt`.

Regeneration
- Install the local toolchain once:
  `cd tools/lv_font_pipeline && npm install`
- Provide the upstream source font:
  `NOTO_SANS_SC_SOURCE_FONT=/abs/path/to/NotoSansSC-Regular.otf`
- Regenerate the tracked output:
  `npm run generate:cjk-12`

Notes
- The glyph list is extracted from the tracked `noto_sans_cjk_12.c` comment block so the
  host-side sanitizer and the device font stay aligned.
- If the supported character set changes, regenerate the font first, then rebuild `tools/esp32dash`
  so its build script refreshes the sanitizer character table.
