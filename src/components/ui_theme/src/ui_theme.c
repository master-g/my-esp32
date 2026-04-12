#include "ui_theme.h"

static lv_theme_t *s_theme;

static const ui_theme_palette_t s_claude_palette = {
    .name = "Claude Dashboard",
    .colors =
        {
            [UI_THEME_COLOR_CANVAS_BG] = 0x14110f,
            [UI_THEME_COLOR_SURFACE_PRIMARY] = 0x1f1a17,
            [UI_THEME_COLOR_SURFACE_SECONDARY] = 0x2b241f,
            [UI_THEME_COLOR_SURFACE_ACTIVE] = 0x4a372d,
            [UI_THEME_COLOR_SURFACE_OVERLAY] = 0x1b1613,
            [UI_THEME_COLOR_BORDER_SUBTLE] = 0x4d433d,
            [UI_THEME_COLOR_TEXT_PRIMARY] = 0xf4f3ee,
            [UI_THEME_COLOR_TEXT_SECONDARY] = 0xd9d2c7,
            [UI_THEME_COLOR_TEXT_MUTED] = 0xb1ada1,
            [UI_THEME_COLOR_TEXT_EMPHASIS] = 0xffffff,
            [UI_THEME_COLOR_ACCENT_PRIMARY] = 0xc15f3c,
            [UI_THEME_COLOR_ACCENT_SOFT] = 0x7a5446,
            [UI_THEME_COLOR_STATUS_SUCCESS] = 0x7fa887,
            [UI_THEME_COLOR_STATUS_WARNING] = 0xc79a55,
            [UI_THEME_COLOR_STATUS_ERROR] = 0xd1765d,
            [UI_THEME_COLOR_STATUS_INFO] = 0x9d9489,
        },
};

const ui_theme_palette_t *ui_theme_get_palette(void) { return &s_claude_palette; }

uint32_t ui_theme_color_hex(ui_theme_color_role_t role)
{
    if (role >= UI_THEME_COLOR_COUNT) {
        return 0x000000;
    }

    return s_claude_palette.colors[role];
}

lv_color_t ui_theme_color(ui_theme_color_role_t role)
{
    return lv_color_hex(ui_theme_color_hex(role));
}

void ui_theme_apply_display(lv_display_t *display)
{
    if (display == NULL) {
        return;
    }

    s_theme =
        lv_theme_default_init(display, ui_theme_color(UI_THEME_COLOR_ACCENT_PRIMARY),
                              ui_theme_color(UI_THEME_COLOR_ACCENT_SOFT), true, LV_FONT_DEFAULT);
    lv_display_set_theme(display, s_theme);
}
