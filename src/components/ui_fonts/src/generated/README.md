# Generated Assets For `ui_fonts`

This directory contains tracked generated LVGL fonts shared by multiple apps.

`departure_mono_11`, `departure_mono_22`, `departure_mono_44`
- Source: Departure Mono v1.500.
- Upstream artifact: `DepartureMono-Regular.otf` from
  `https://github.com/rektdeckard/departure-mono/releases/tag/v1.500`
- License: SIL Open Font License 1.1.
  See `third_party/fonts/departure-mono/OFL-1.1.txt`.

`noto_sans_cjk_11`, `noto_sans_cjk_22`
- Purpose: CJK fallback fonts paired with Departure Mono text sizes.
- Upstream font family: Noto Sans CJK SC / Noto Sans SC.
- License: SIL Open Font License 1.1.
  See `third_party/fonts/noto-sans-cjk/OFL-1.1.txt`.

Regeneration
- Install the local toolchain once:
  `cd tools/lv_font_pipeline && npm install`
- Generate Departure Mono:
  `npm run generate:departure`
- Provide the upstream Noto OTF:
  `NOTO_SANS_SC_SOURCE_FONT=/abs/path/to/NotoSansSC-Regular.otf`
- Generate the shared CJK fallback fonts:
  `npm run generate:cjk-11 && npm run generate:cjk-22`
