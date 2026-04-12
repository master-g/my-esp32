#include "trading_view.h"

#include <string.h>

#include "bsp_board_config.h"
#include "service_market.h"
#include "ui_theme.h"
#include "ui_fonts.h"

#define TRADING_BG ui_theme_color_hex(UI_THEME_COLOR_CANVAS_BG)
#define TRADING_PANEL ui_theme_color_hex(UI_THEME_COLOR_SURFACE_PRIMARY)
#define TRADING_PANEL_ALT ui_theme_color_hex(UI_THEME_COLOR_SURFACE_SECONDARY)
#define TRADING_PANEL_ACTIVE ui_theme_color_hex(UI_THEME_COLOR_SURFACE_ACTIVE)
#define TRADING_TEXT ui_theme_color_hex(UI_THEME_COLOR_TEXT_PRIMARY)
#define TRADING_MUTED ui_theme_color_hex(UI_THEME_COLOR_TEXT_MUTED)
#define TRADING_BORDER ui_theme_color_hex(UI_THEME_COLOR_BORDER_SUBTLE)
#define TRADING_CHIP_BG ui_theme_color_hex(UI_THEME_COLOR_SURFACE_SECONDARY)

static lv_obj_t *create_segment_button(lv_obj_t *parent, const char *text, lv_obj_t **label_out)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_t *label = lv_label_create(btn);

    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(TRADING_PANEL_ALT), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(TRADING_BORDER), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_hor(btn, 10, 0);
    lv_obj_set_style_pad_ver(btn, 4, 0);
    lv_obj_set_style_bg_color(btn, ui_theme_color(UI_THEME_COLOR_ACCENT_SOFT), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(btn, lv_color_hex(TRADING_TEXT), 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_style_text_font(label, ui_font_text_11(), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(TRADING_TEXT), 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);

    if (label_out != NULL) {
        *label_out = label;
    }

    return btn;
}

static void apply_segment_state(lv_obj_t *btn, lv_obj_t *label, bool selected)
{
    if (btn == NULL || label == NULL) {
        return;
    }

    lv_obj_set_style_bg_color(btn,
                              lv_color_hex(selected ? TRADING_PANEL_ACTIVE : TRADING_PANEL_ALT), 0);
    lv_obj_set_style_border_color(
        btn,
        selected ? ui_theme_color(UI_THEME_COLOR_ACCENT_PRIMARY) : lv_color_hex(TRADING_BORDER), 0);
    lv_obj_set_style_text_color(
        label,
        selected ? ui_theme_color(UI_THEME_COLOR_TEXT_EMPHASIS) : lv_color_hex(TRADING_MUTED), 0);
}

lv_obj_t *trading_view_create(trading_view_t *view, lv_obj_t *parent)
{
    static const lv_coord_t pair_button_x[MARKET_PAIR_COUNT] = {0, 82, 164};
    static const lv_coord_t interval_button_x[MARKET_INTERVAL_COUNT] = {18, 98, 178};
    uint32_t i = 0;

    if (view == NULL) {
        return NULL;
    }

    memset(view, 0, sizeof(*view));
    view->root = lv_obj_create(parent);
    lv_obj_remove_style_all(view->root);
    lv_obj_set_size(view->root, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(view->root, lv_color_hex(TRADING_BG), 0);
    lv_obj_set_style_bg_opa(view->root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(view->root, LV_OBJ_FLAG_SCROLLABLE);

    for (i = 0; i < MARKET_PAIR_COUNT; ++i) {
        view->pair_buttons[i] = create_segment_button(
            view->root, market_pair_label((market_pair_id_t)i), &view->pair_labels[i]);
        lv_obj_set_size(view->pair_buttons[i], 78, 30);
        lv_obj_set_pos(view->pair_buttons[i], 14 + pair_button_x[i], 12);
    }

    view->transport_chip = lv_obj_create(view->root);
    lv_obj_remove_style_all(view->transport_chip);
    lv_obj_set_size(view->transport_chip, 60, 24);
    lv_obj_set_pos(view->transport_chip, BSP_LCD_H_RES - 74, 14);
    lv_obj_set_style_radius(view->transport_chip, 12, 0);
    lv_obj_set_style_bg_color(view->transport_chip, lv_color_hex(TRADING_CHIP_BG), 0);
    lv_obj_set_style_bg_opa(view->transport_chip, LV_OPA_COVER, 0);

    view->transport_label = lv_label_create(view->transport_chip);
    lv_obj_set_style_text_font(view->transport_label, ui_font_text_11(), 0);
    lv_obj_set_style_text_color(view->transport_label, lv_color_hex(TRADING_MUTED), 0);
    lv_label_set_text(view->transport_label, "POLL");
    lv_obj_center(view->transport_label);

    view->price_label = lv_label_create(view->root);
    lv_obj_set_style_text_font(view->price_label, ui_font_display_44(), 0);
    lv_obj_set_style_text_color(view->price_label, lv_color_hex(TRADING_TEXT), 0);
    lv_label_set_text(view->price_label, "--");
    lv_obj_set_pos(view->price_label, 16, 52);

    view->change_label = lv_label_create(view->root);
    lv_obj_set_style_text_font(view->change_label, ui_font_text_22(), 0);
    lv_obj_set_style_text_color(view->change_label, lv_color_hex(TRADING_MUTED), 0);
    lv_label_set_text(view->change_label, "--");
    lv_obj_set_pos(view->change_label, 18, 104);

    view->status_label = lv_label_create(view->root);
    lv_obj_set_width(view->status_label, 238);
    lv_obj_set_style_text_font(view->status_label, ui_font_text_11(), 0);
    lv_obj_set_style_text_color(view->status_label, lv_color_hex(TRADING_MUTED), 0);
    lv_label_set_long_mode(view->status_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(view->status_label, "Waiting for first tick");
    lv_obj_set_pos(view->status_label, 18, 135);

    view->chart_panel = lv_obj_create(view->root);
    lv_obj_remove_style_all(view->chart_panel);
    lv_obj_set_size(view->chart_panel, 350, 148);
    lv_obj_set_pos(view->chart_panel, 276, 12);
    lv_obj_set_style_radius(view->chart_panel, 18, 0);
    lv_obj_set_style_bg_color(view->chart_panel, lv_color_hex(TRADING_PANEL), 0);
    lv_obj_set_style_bg_opa(view->chart_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(view->chart_panel, lv_color_hex(TRADING_BORDER), 0);
    lv_obj_set_style_border_width(view->chart_panel, 1, 0);
    lv_obj_set_style_pad_all(view->chart_panel, 0, 0);
    lv_obj_clear_flag(view->chart_panel, LV_OBJ_FLAG_SCROLLABLE);

    trading_chart_create(&view->chart, view->chart_panel);
    lv_obj_set_size(view->chart.root, 330, 92);
    lv_obj_set_pos(view->chart.root, 10, 12);

    for (i = 0; i < MARKET_INTERVAL_COUNT; ++i) {
        view->interval_buttons[i] =
            create_segment_button(view->chart_panel, market_interval_label((market_interval_id_t)i),
                                  &view->interval_labels[i]);
        lv_obj_set_size(view->interval_buttons[i], 68, 28);
        lv_obj_set_pos(view->interval_buttons[i], interval_button_x[i], 110);
    }

    return view->root;
}

void trading_view_apply(trading_view_t *view, const trading_present_model_t *model)
{
    uint32_t i = 0;

    if (view == NULL || model == NULL) {
        return;
    }

    lv_label_set_text(view->price_label, model->price_text);
    lv_obj_set_style_text_color(view->price_label, lv_color_hex(model->price_color), 0);
    lv_label_set_text(view->change_label, model->change_text);
    lv_obj_set_style_text_color(view->change_label, lv_color_hex(model->change_color), 0);
    lv_label_set_text(view->status_label, model->status_text);
    lv_obj_set_style_text_color(view->status_label, lv_color_hex(model->status_color), 0);
    lv_label_set_text(view->transport_label, model->transport_text);
    lv_obj_set_style_text_color(view->transport_label, lv_color_hex(model->transport_color), 0);

    for (i = 0; i < MARKET_PAIR_COUNT; ++i) {
        apply_segment_state(view->pair_buttons[i], view->pair_labels[i], model->pair_selected[i]);
    }

    for (i = 0; i < MARKET_INTERVAL_COUNT; ++i) {
        apply_segment_state(view->interval_buttons[i], view->interval_labels[i],
                            model->interval_selected[i]);
    }

    lv_obj_set_style_border_color(view->chart_panel,
                                  model->chart_dimmed
                                      ? ui_theme_color(UI_THEME_COLOR_STATUS_WARNING)
                                      : lv_color_hex(TRADING_BORDER),
                                  0);
    trading_chart_apply(&view->chart, model);
}

void trading_view_set_hidden(trading_view_t *view, bool hidden)
{
    if (view == NULL || view->root == NULL) {
        return;
    }

    if (hidden) {
        lv_obj_add_flag(view->root, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(view->root, LV_OBJ_FLAG_HIDDEN);
    }
}
