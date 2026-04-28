#include "home_prompt.h"

#include <stdio.h>
#include <string.h>

#include "bsp_board_config.h"
#include "device_link.h"
#include "home_internal.h"
#include "ui_fonts.h"
#include "ui_theme.h"

#define PROMPT_TITLE_W 132
#define PROMPT_TEXT_X 140
#define PROMPT_TEXT_W (BSP_LCD_H_RES - 32 - PROMPT_TEXT_X)
#define PROMPT_MAX_VISIBLE_OPTIONS 4

static void prompt_btn_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target_obj(e);
    lv_obj_t *overlay = NULL;
    uint8_t index = (uint8_t)(uintptr_t)lv_event_get_user_data(e);

    if (btn != NULL) {
        lv_obj_t *btn_row = lv_obj_get_parent(btn);
        overlay = btn_row != NULL ? lv_obj_get_parent(btn_row) : NULL;
    }
    device_link_resolve_prompt(index);
    if (overlay != NULL) {
        lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

static void clear_option_buttons(home_prompt_t *prompt)
{
    for (int i = 0; i < PROMPT_MAX_VISIBLE_OPTIONS; i++) {
        if (prompt->option_btns[i] != NULL) {
            lv_obj_delete(prompt->option_btns[i]);
            prompt->option_btns[i] = NULL;
        }
    }
    prompt->option_count = 0;
}

static lv_obj_t *create_option_btn(lv_obj_t *parent, uint8_t index, const char *label)
{
    char label_text[16];
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_t *text_label;

    if (label != NULL && label[0] != '\0') {
        // Truncate to max 10 chars + ellipsis to fit button width
        if (strlen(label) > 10) {
            snprintf(label_text, sizeof(label_text), "%.10s…", label);
        } else {
            strlcpy(label_text, label, sizeof(label_text));
        }
    } else {
        snprintf(label_text, sizeof(label_text), "%u", (unsigned int)(index + 1));
    }

    lv_obj_set_height(btn, 32);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_bg_color(btn, lv_color_hex(ui_theme_color_hex(UI_THEME_COLOR_STATUS_INFO)), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_pad_hor(btn, 4, 0);
    lv_obj_set_style_pad_ver(btn, 0, 0);

    text_label = lv_label_create(btn);
    lv_label_set_text(text_label, label_text);
    lv_obj_set_style_text_font(text_label, ui_font_text_11(), 0);
    lv_obj_set_style_text_color(text_label, lv_color_white(), 0);
    lv_obj_center(text_label);

    lv_obj_add_event_cb(btn, prompt_btn_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)index);
    return btn;
}

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

    prompt->btn_row = lv_obj_create(prompt->overlay);
    lv_obj_remove_style_all(prompt->btn_row);
    lv_obj_set_size(prompt->btn_row, PROMPT_TEXT_W, 36);
    lv_obj_align(prompt->btn_row, LV_ALIGN_BOTTOM_LEFT, PROMPT_TEXT_X, -4);
    lv_obj_set_flex_flow(prompt->btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(prompt->btn_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(prompt->btn_row, 8, 0);
    lv_obj_add_flag(prompt->btn_row, LV_OBJ_FLAG_HIDDEN);
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

    clear_option_buttons(prompt);

    lv_label_set_text(prompt->title_label, req.title[0] != '\0' ? req.title : "Awaiting input");
    lv_label_set_text(prompt->question_label,
                      req.question[0] != '\0' ? req.question : "Check terminal for input");

    if (req.option_count > 0) {
        uint8_t visible = req.option_count > PROMPT_MAX_VISIBLE_OPTIONS ? PROMPT_MAX_VISIBLE_OPTIONS
                                                                        : req.option_count;
        lv_label_set_text(prompt->options_label, req.options_text);
        lv_obj_clear_flag(prompt->options_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(prompt->hint_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(prompt->btn_row, LV_OBJ_FLAG_HIDDEN);

        for (uint8_t i = 0; i < visible; i++) {
            const char *label = (i < req.option_count && req.option_labels[i][0] != '\0')
                                    ? req.option_labels[i]
                                    : NULL;
            prompt->option_btns[i] = create_option_btn(prompt->btn_row, i, label);
        }
        prompt->option_count = visible;
    } else {
        lv_label_set_text(prompt->options_label, "");
        lv_obj_add_flag(prompt->options_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(prompt->hint_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(prompt->btn_row, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_clear_flag(prompt->overlay, LV_OBJ_FLAG_HIDDEN);
}

void home_prompt_hide(home_prompt_t *prompt)
{
    if (prompt != NULL && prompt->overlay != NULL) {
        clear_option_buttons(prompt);
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
