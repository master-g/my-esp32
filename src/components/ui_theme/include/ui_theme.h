#ifndef UI_THEME_H
#define UI_THEME_H

#include <stdint.h>

#include "lvgl.h"

typedef enum {
    UI_THEME_COLOR_CANVAS_BG = 0,
    UI_THEME_COLOR_SURFACE_PRIMARY,
    UI_THEME_COLOR_SURFACE_SECONDARY,
    UI_THEME_COLOR_SURFACE_ACTIVE,
    UI_THEME_COLOR_SURFACE_OVERLAY,
    UI_THEME_COLOR_BORDER_SUBTLE,
    UI_THEME_COLOR_TEXT_PRIMARY,
    UI_THEME_COLOR_TEXT_SECONDARY,
    UI_THEME_COLOR_TEXT_MUTED,
    UI_THEME_COLOR_TEXT_EMPHASIS,
    UI_THEME_COLOR_ACCENT_PRIMARY,
    UI_THEME_COLOR_ACCENT_SOFT,
    UI_THEME_COLOR_STATUS_SUCCESS,
    UI_THEME_COLOR_STATUS_WARNING,
    UI_THEME_COLOR_STATUS_ERROR,
    UI_THEME_COLOR_STATUS_INFO,
    UI_THEME_COLOR_COUNT,
} ui_theme_color_role_t;

typedef struct {
    const char *name;
    uint32_t colors[UI_THEME_COLOR_COUNT];
} ui_theme_palette_t;

const ui_theme_palette_t *ui_theme_get_palette(void);
uint32_t ui_theme_color_hex(ui_theme_color_role_t role);
lv_color_t ui_theme_color(ui_theme_color_role_t role);
void ui_theme_apply_display(lv_display_t *display);

#endif
