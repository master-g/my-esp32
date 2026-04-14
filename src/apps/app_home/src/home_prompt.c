#include "home_prompt.h"

#include <string.h>

#include "bsp_board_config.h"
#include "device_link.h"
#include "home_internal.h"
#include "ui_fonts.h"
#include "ui_theme.h"

#define PROMPT_TITLE_W 132
#define PROMPT_TEXT_X 140
#define PROMPT_TEXT_W (BSP_LCD_H_RES - 32 - PROMPT_TEXT_X)

void home_prompt_create(home_prompt_t *prompt, lv_obj_t *root)
{
    if (prompt == NULL || root == NULL) {
        return;
    }

    memset(prompt, 0, sizeof(*prompt));

    prompt->overlay = lv_obj_create(root);
    lv_obj_remove_style_all(prompt->overlay);
    lv_obj_set_size(prompt->overlay, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(prompt->overlay, lv_color_hex(APPROVE_BG_COLOR), 0);
    lv_obj_set_style_bg_opa(prompt->overlay, LV_OPA_90, 0);
    lv_obj_set_style_pad_hor(prompt->overlay, 16, 0);
    lv_obj_set_style_pad_ver(prompt->overlay, 10, 0);
    lv_obj_add_flag(prompt->overlay, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_FLOATING);
    lv_obj_align(prompt->overlay, LV_ALIGN_TOP_LEFT, -16, -16);

    prompt->title_label = lv_label_create(prompt->overlay);
    lv_obj_set_style_text_font(prompt->title_label, ui_font_text_11(), 0);
    lv_obj_set_style_text_color(prompt->title_label, ui_theme_color(UI_THEME_COLOR_TEXT_PRIMARY),
                                0);
    lv_label_set_long_mode(prompt->title_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(prompt->title_label, PROMPT_TITLE_W);
    lv_obj_align(prompt->title_label, LV_ALIGN_TOP_LEFT, 0, 8);

    prompt->question_label = lv_label_create(prompt->overlay);
    lv_obj_set_style_text_font(prompt->question_label, ui_font_text_11(), 0);
    lv_obj_set_style_text_color(prompt->question_label, ui_theme_color(UI_THEME_COLOR_TEXT_PRIMARY),
                                0);
    lv_label_set_long_mode(prompt->question_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(prompt->question_label, PROMPT_TEXT_W);
    lv_obj_align(prompt->question_label, LV_ALIGN_TOP_LEFT, PROMPT_TEXT_X, 8);

    prompt->options_label = lv_label_create(prompt->overlay);
    lv_obj_set_style_text_font(prompt->options_label, ui_font_text_11(), 0);
    lv_obj_set_style_text_color(prompt->options_label, ui_theme_color(UI_THEME_COLOR_TEXT_MUTED),
                                0);
    lv_label_set_long_mode(prompt->options_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(prompt->options_label, PROMPT_TEXT_W);
    lv_obj_align(prompt->options_label, LV_ALIGN_TOP_LEFT, PROMPT_TEXT_X, 56);
    lv_obj_add_flag(prompt->options_label, LV_OBJ_FLAG_HIDDEN);

    prompt->hint_label = lv_label_create(prompt->overlay);
    lv_obj_set_style_text_font(prompt->hint_label, ui_font_text_11(), 0);
    lv_obj_set_style_text_color(prompt->hint_label, ui_theme_color(UI_THEME_COLOR_TEXT_MUTED), 0);
    lv_label_set_text(prompt->hint_label, "Answer in terminal");
    lv_obj_align(prompt->hint_label, LV_ALIGN_BOTTOM_LEFT, PROMPT_TEXT_X, 0);
}

void home_prompt_show_pending(home_prompt_t *prompt)
{
    prompt_request_t req;

    if (prompt == NULL || prompt->overlay == NULL) {
        return;
    }
    if (!device_link_get_pending_prompt(&req)) {
        return;
    }

    lv_label_set_text(prompt->title_label, req.title[0] != '\0' ? req.title : "Awaiting input");
    lv_label_set_text(prompt->question_label,
                      req.question[0] != '\0' ? req.question : "Check terminal for input");
    if (req.options_text[0] != '\0') {
        lv_label_set_text(prompt->options_label, req.options_text);
        lv_obj_clear_flag(prompt->options_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(prompt->options_label, "");
        lv_obj_add_flag(prompt->options_label, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(prompt->overlay, LV_OBJ_FLAG_HIDDEN);
}

void home_prompt_hide(home_prompt_t *prompt)
{
    if (prompt != NULL && prompt->overlay != NULL) {
        lv_obj_add_flag(prompt->overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

void home_prompt_on_connection_changed(home_prompt_t *prompt, bool was_connected, bool is_connected)
{
    if (prompt == NULL || !was_connected || is_connected || !home_prompt_is_visible(prompt)) {
        return;
    }

    device_link_dismiss_prompt();
    home_prompt_hide(prompt);
}

bool home_prompt_is_visible(const home_prompt_t *prompt)
{
    return prompt != NULL && prompt->overlay != NULL &&
           !lv_obj_has_flag(prompt->overlay, LV_OBJ_FLAG_HIDDEN);
}
