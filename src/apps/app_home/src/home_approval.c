#include "home_approval.h"

#include <stdint.h>
#include <string.h>

#include "bsp_board_config.h"
#include "device_link.h"
#include "home_internal.h"
#include "ui_theme.h"
#include "ui_fonts.h"

static void approval_btn_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target_obj(e);
    lv_obj_t *overlay = NULL;
    approval_decision_t decision = (approval_decision_t)(intptr_t)lv_event_get_user_data(e);

    if (btn != NULL) {
        lv_obj_t *btn_row = lv_obj_get_parent(btn);
        overlay = btn_row != NULL ? lv_obj_get_parent(btn_row) : NULL;
    }
    device_link_resolve_approval(decision);
    if (overlay != NULL) {
        lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *create_approval_btn(lv_obj_t *parent, const char *icon, const char *text,
                                     uint32_t color, approval_decision_t decision)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_t *icon_label;
    lv_obj_t *text_label;

    lv_obj_set_height(btn, APPROVE_BTN_H);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_pad_hor(btn, 10, 0);
    lv_obj_set_style_pad_ver(btn, 0, 0);
    lv_obj_set_style_pad_column(btn, 6, 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    icon_label = lv_label_create(btn);
    lv_label_set_text(icon_label, icon);
    lv_obj_set_style_text_font(icon_label, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(icon_label, lv_color_white(), 0);

    text_label = lv_label_create(btn);
    lv_label_set_text(text_label, text);
    lv_obj_set_style_text_font(text_label, ui_font_text_11(), 0);
    lv_obj_set_style_text_color(text_label, lv_color_white(), 0);

    lv_obj_add_event_cb(btn, approval_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)decision);
    return btn;
}

void home_approval_create(home_approval_t *approval, lv_obj_t *root)
{
    lv_obj_t *btn_row;

    if (approval == NULL || root == NULL) {
        return;
    }

    memset(approval, 0, sizeof(*approval));

    approval->overlay = lv_obj_create(root);
    lv_obj_remove_style_all(approval->overlay);
    lv_obj_set_size(approval->overlay, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(approval->overlay, lv_color_hex(APPROVE_BG_COLOR), 0);
    lv_obj_set_style_bg_opa(approval->overlay, LV_OPA_90, 0);
    lv_obj_set_style_pad_hor(approval->overlay, 16, 0);
    lv_obj_set_style_pad_ver(approval->overlay, 10, 0);
    lv_obj_add_flag(approval->overlay, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_FLOATING);
    lv_obj_align(approval->overlay, LV_ALIGN_TOP_LEFT, -16, -16);

    approval->tool_label = lv_label_create(approval->overlay);
    lv_obj_set_style_text_font(approval->tool_label, ui_font_text_11(), 0);
    lv_obj_set_style_text_color(approval->tool_label, ui_theme_color(UI_THEME_COLOR_TEXT_PRIMARY),
                                0);
    lv_label_set_long_mode(approval->tool_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(approval->tool_label, 132);
    lv_obj_align(approval->tool_label, LV_ALIGN_TOP_LEFT, 0, 8);

    approval->desc_label = lv_label_create(approval->overlay);
    lv_obj_set_style_text_font(approval->desc_label, ui_font_text_11(), 0);
    lv_obj_set_style_text_color(approval->desc_label, ui_theme_color(UI_THEME_COLOR_TEXT_MUTED), 0);
    lv_obj_set_width(approval->desc_label, BSP_LCD_H_RES - 32 - 140);
    lv_label_set_long_mode(approval->desc_label, LV_LABEL_LONG_DOT);
    lv_obj_align(approval->desc_label, LV_ALIGN_TOP_LEFT, 140, 10);

    btn_row = lv_obj_create(approval->overlay);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_size(btn_row, BSP_LCD_H_RES - 32, BSP_LCD_V_RES - APPROVE_INFO_H - 20);
    lv_obj_align(btn_row, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_row, APPROVE_BTN_GAP, 0);

    approval->btn_allow = create_approval_btn(btn_row, LV_SYMBOL_OK, "Accept", APPROVE_ALLOW_COLOR,
                                              APPROVAL_DECISION_ALLOW);
    approval->btn_deny = create_approval_btn(btn_row, LV_SYMBOL_CLOSE, "Decline",
                                             APPROVE_DENY_COLOR, APPROVAL_DECISION_DENY);
    approval->btn_yolo = create_approval_btn(btn_row, LV_SYMBOL_WARNING, "YOLO", APPROVE_YOLO_COLOR,
                                             APPROVAL_DECISION_YOLO);
}

void home_approval_show_pending(home_approval_t *approval)
{
    approval_request_t req;

    if (approval == NULL || approval->overlay == NULL) {
        return;
    }
    if (!device_link_get_pending_approval(&req)) {
        return;
    }

    lv_label_set_text(approval->tool_label, req.tool_name);
    lv_label_set_text(approval->desc_label, req.description);
    lv_obj_clear_flag(approval->overlay, LV_OBJ_FLAG_HIDDEN);
}

void home_approval_hide(home_approval_t *approval)
{
    if (approval != NULL && approval->overlay != NULL) {
        lv_obj_add_flag(approval->overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

void home_approval_on_connection_changed(home_approval_t *approval, bool was_connected,
                                         bool is_connected)
{
    if (approval == NULL || !was_connected || is_connected || !home_approval_is_visible(approval)) {
        return;
    }

    device_link_cancel_approval();
    home_approval_hide(approval);
}

bool home_approval_is_visible(const home_approval_t *approval)
{
    return approval != NULL && approval->overlay != NULL &&
           !lv_obj_has_flag(approval->overlay, LV_OBJ_FLAG_HIDDEN);
}
