#include "home_view.h"

#include <string.h>

#include "bsp_board_config.h"
#include "generated/app_home_status_font.h"
#include "generated/sprite_frames.h"
#include "home_internal.h"
#include "ui_fonts.h"

struct sprite_anim_def_t {
    const lv_image_dsc_t *frames;
    uint8_t num_frames;
    uint16_t period_ms;
};

static const sprite_anim_def_t s_sprite_anims[SPRITE_STATE_COUNT] = {
    [SPRITE_STATE_IDLE] = {sprite_idle_frames, SPRITE_FRAMES_PER_STATE, 333},
    [SPRITE_STATE_WORKING] = {sprite_working_frames, SPRITE_FRAMES_PER_STATE, 250},
    [SPRITE_STATE_WAITING] = {sprite_waiting_frames, SPRITE_FRAMES_PER_STATE, 333},
    [SPRITE_STATE_SLEEPING] = {sprite_sleeping_frames, SPRITE_FRAMES_PER_STATE, 500},
};

static const sprite_anim_def_t *current_anim(const home_view_t *view) { return view->sprite_anim; }

static void set_status_dot_visible(lv_obj_t *dot, bool visible)
{
    if (dot == NULL) {
        return;
    }

    if (visible) {
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
    }
}

static void stop_bubble_timer(home_view_t *view)
{
    if (view->bubble_timer != NULL) {
        lv_timer_delete(view->bubble_timer);
        view->bubble_timer = NULL;
    }
}

static void sprite_timer_cb(lv_timer_t *timer)
{
    home_view_t *view = lv_timer_get_user_data(timer);
    const sprite_anim_def_t *anim;

    if (view == NULL || view->sprite_img == NULL) {
        return;
    }

    anim = current_anim(view);
    if (anim == NULL) {
        return;
    }

    view->frame_idx = (uint8_t)((view->frame_idx + 1U) % anim->num_frames);
    lv_image_set_src(view->sprite_img, &anim->frames[view->frame_idx]);
}

static void bubble_fade_cb(lv_timer_t *timer)
{
    home_view_t *view = lv_timer_get_user_data(timer);

    if (view == NULL) {
        return;
    }

    view->bubble_dismissed = true;
    if (view->bubble_box != NULL) {
        lv_obj_add_flag(view->bubble_box, LV_OBJ_FLAG_HIDDEN);
    }
    stop_bubble_timer(view);
}

static void refresh_sprite(home_view_t *view, sprite_state_t new_state)
{
    const sprite_anim_def_t *anim;

    if (view->sprite_img == NULL) {
        return;
    }

    if (new_state == view->sprite_state && current_anim(view) != NULL) {
        return;
    }

    anim = &s_sprite_anims[new_state];
    view->sprite_state = new_state;
    view->sprite_anim = anim;
    view->frame_idx = 0;
    lv_image_set_src(view->sprite_img, &anim->frames[0]);
    if (view->sprite_timer != NULL) {
        lv_timer_set_period(view->sprite_timer, anim->period_ms);
    }
}

static void refresh_bubble(home_view_t *view, const home_present_model_t *model)
{
    bool text_changed;
    const char *current;

    if (view->bubble_box == NULL || model == NULL) {
        return;
    }

    if (!model->bubble_visible) {
        lv_obj_add_flag(view->bubble_box, LV_OBJ_FLAG_HIDDEN);
        view->bubble_dismissed = false;
        stop_bubble_timer(view);
        return;
    }

    current = lv_label_get_text(view->bubble_label);
    text_changed = current == NULL || strcmp(current, model->bubble_text) != 0;

    if (text_changed) {
        view->bubble_dismissed = false;
        lv_label_set_text(view->bubble_label, model->bubble_text);
        lv_obj_clear_flag(view->bubble_box, LV_OBJ_FLAG_HIDDEN);
        stop_bubble_timer(view);
        view->bubble_timer = lv_timer_create(bubble_fade_cb, HOME_BUBBLE_FADE_MS, view);
        lv_timer_set_repeat_count(view->bubble_timer, 1);
        return;
    }

    if (!view->bubble_dismissed) {
        lv_obj_clear_flag(view->bubble_box, LV_OBJ_FLAG_HIDDEN);
    }
}

lv_obj_t *home_view_create(home_view_t *view, lv_obj_t *parent)
{
    if (view == NULL) {
        return NULL;
    }

    memset(view, 0, sizeof(*view));

    view->root = lv_obj_create(parent);
    lv_obj_remove_style_all(view->root);
    lv_obj_set_size(view->root, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(view->root, lv_color_hex(HOME_BG_BASE_COLOR), 0);
    lv_obj_set_style_bg_opa(view->root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(view->root, 16, 0);

    view->status_bar = lv_obj_create(view->root);
    lv_obj_remove_style_all(view->status_bar);
    lv_obj_set_size(view->status_bar, HOME_STATUS_BAR_WIDTH, HOME_STATUS_BAR_HEIGHT);
    lv_obj_align(view->status_bar, LV_ALIGN_TOP_LEFT, 0, HOME_STATUS_BAR_Y);
    lv_obj_set_flex_flow(view->status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(view->status_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(view->status_bar, HOME_STATUS_ICON_GAP, 0);

    view->wifi_item = lv_obj_create(view->status_bar);
    lv_obj_remove_style_all(view->wifi_item);
    lv_obj_set_size(view->wifi_item, HOME_STATUS_ITEM_BOX_SIZE, HOME_STATUS_ITEM_BOX_SIZE);

    view->wifi_icon = lv_label_create(view->wifi_item);
    lv_label_set_text_static(view->wifi_icon, APP_HOME_SYMBOL_WIFI);
    lv_obj_set_style_text_font(view->wifi_icon, &app_home_status_font, 0);
    lv_obj_align(view->wifi_icon, LV_ALIGN_CENTER, 0, 0);

    view->wifi_dot = lv_obj_create(view->wifi_item);
    lv_obj_remove_style_all(view->wifi_dot);
    lv_obj_set_size(view->wifi_dot, 4, 4);
    lv_obj_set_style_bg_color(view->wifi_dot, lv_color_hex(HOME_WIFI_CONNECTING_DOT_COLOR), 0);
    lv_obj_set_style_bg_opa(view->wifi_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(view->wifi_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(view->wifi_dot, LV_ALIGN_TOP_RIGHT, 0, 1);
    lv_obj_add_flag(view->wifi_dot, LV_OBJ_FLAG_HIDDEN);

    view->claude_item = lv_obj_create(view->status_bar);
    lv_obj_remove_style_all(view->claude_item);
    lv_obj_set_size(view->claude_item, HOME_STATUS_ITEM_BOX_SIZE, HOME_STATUS_ITEM_BOX_SIZE);

    view->claude_icon = lv_label_create(view->claude_item);
    lv_label_set_text_static(view->claude_icon, APP_HOME_SYMBOL_CLAUDE);
    lv_obj_set_style_text_font(view->claude_icon, &app_home_status_font, 0);
    lv_obj_align(view->claude_icon, LV_ALIGN_CENTER, 0, 0);

    view->claude_dot = lv_obj_create(view->claude_item);
    lv_obj_remove_style_all(view->claude_dot);
    lv_obj_set_size(view->claude_dot, 4, 4);
    lv_obj_set_style_bg_color(view->claude_dot, lv_color_hex(HOME_CLAUDE_UNREAD_DOT_COLOR), 0);
    lv_obj_set_style_bg_opa(view->claude_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(view->claude_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(view->claude_dot, LV_ALIGN_TOP_RIGHT, 0, 1);
    lv_obj_add_flag(view->claude_dot, LV_OBJ_FLAG_HIDDEN);

    view->settings_item = lv_button_create(view->status_bar);
    lv_obj_remove_style_all(view->settings_item);
    lv_obj_set_size(view->settings_item, HOME_STATUS_ACTION_W, HOME_STATUS_ITEM_BOX_SIZE);
    lv_obj_set_style_bg_color(view->settings_item, lv_color_hex(HOME_STATUS_ACTION_BG_COLOR), 0);
    lv_obj_set_style_bg_opa(view->settings_item, LV_OPA_70, 0);
    lv_obj_set_style_radius(view->settings_item, 4, 0);
    lv_obj_set_style_border_width(view->settings_item, 0, 0);

    {
        lv_obj_t *settings_label = lv_label_create(view->settings_item);
        lv_label_set_text_static(settings_label, "SET");
        lv_obj_set_style_text_font(settings_label, ui_font_text_11(), 0);
        lv_obj_set_style_text_color(settings_label, lv_color_hex(HOME_STATUS_ACTION_TEXT_COLOR), 0);
        lv_obj_center(settings_label);
    }

    view->time_label = lv_label_create(view->root);
    lv_obj_set_style_text_font(view->time_label, ui_font_display_44(), 0);
    lv_obj_set_style_text_color(view->time_label, ui_theme_color(UI_THEME_COLOR_TEXT_EMPHASIS), 0);
    lv_obj_align(view->time_label, LV_ALIGN_TOP_LEFT, 0, HOME_TIME_Y);

    view->date_label = lv_label_create(view->root);
    lv_obj_set_style_text_font(view->date_label, ui_font_text_22(), 0);
    lv_obj_set_style_text_color(view->date_label, ui_theme_color(UI_THEME_COLOR_TEXT_SECONDARY), 0);
    lv_obj_align(view->date_label, LV_ALIGN_TOP_LEFT, 0, HOME_DATE_Y);

    view->weather_row = lv_obj_create(view->root);
    lv_obj_remove_style_all(view->weather_row);
    lv_obj_set_size(view->weather_row, HOME_LEFT_HALF_W, HOME_WEATHER_ROW_HEIGHT);
    lv_obj_align(view->weather_row, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_flex_flow(view->weather_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(view->weather_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(view->weather_row, HOME_WEATHER_ICON_GAP, 0);

    view->weather_icon = lv_label_create(view->weather_row);
    lv_label_set_text_static(view->weather_icon, APP_HOME_SYMBOL_WEATHER_UNKNOWN);
    lv_obj_set_style_text_font(view->weather_icon, &app_home_status_font, 0);
    lv_obj_set_style_translate_y(view->weather_icon, HOME_WEATHER_ICON_Y_OFFSET, 0);

    view->weather_label = lv_label_create(view->weather_row);
    lv_obj_set_width(view->weather_label,
                     HOME_LEFT_HALF_W - HOME_STATUS_ITEM_BOX_SIZE - HOME_WEATHER_ICON_GAP);
    lv_label_set_long_mode(view->weather_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(view->weather_label, ui_font_text_11(), 0);
    lv_obj_set_style_text_color(view->weather_label, lv_color_hex(HOME_WEATHER_TEXT_COLOR), 0);
    lv_obj_set_style_translate_y(view->weather_label, HOME_WEATHER_TEXT_Y_OFFSET, 0);

    view->sprite_img = lv_image_create(view->root);
    lv_image_set_src(view->sprite_img, &sprite_idle_frames[0]);
    lv_image_set_scale(view->sprite_img, HOME_SPRITE_SCALE);
    lv_obj_set_style_image_recolor(view->sprite_img, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(view->sprite_img, LV_OPA_TRANSP, 0);
    lv_obj_align(view->sprite_img, LV_ALIGN_RIGHT_MID, -60, 0);

    view->bubble_box = lv_obj_create(view->root);
    lv_obj_remove_style_all(view->bubble_box);
    lv_obj_set_size(view->bubble_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(view->bubble_box, HOME_BUBBLE_MAX_W, 0);
    lv_obj_set_style_bg_color(view->bubble_box, lv_color_hex(HOME_BUBBLE_BG_COLOR), 0);
    lv_obj_set_style_bg_opa(view->bubble_box, LV_OPA_80, 0);
    lv_obj_set_style_radius(view->bubble_box, HOME_BUBBLE_RADIUS, 0);
    lv_obj_set_style_pad_hor(view->bubble_box, HOME_BUBBLE_PAD_H, 0);
    lv_obj_set_style_pad_ver(view->bubble_box, HOME_BUBBLE_PAD_V, 0);
    lv_obj_align(view->bubble_box, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_flag(view->bubble_box, LV_OBJ_FLAG_HIDDEN);

    view->bubble_label = lv_label_create(view->bubble_box);
    lv_obj_set_style_max_width(view->bubble_label, HOME_BUBBLE_MAX_W - 2 * HOME_BUBBLE_PAD_H, 0);
    lv_label_set_long_mode(view->bubble_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(view->bubble_label, ui_font_text_11(), 0);
    lv_obj_set_style_text_color(view->bubble_label, lv_color_hex(HOME_BUBBLE_TEXT_COLOR), 0);
    lv_label_set_text_static(view->bubble_label, "");

    view->sprite_state = SPRITE_STATE_IDLE;
    view->sprite_anim = &s_sprite_anims[SPRITE_STATE_IDLE];
    view->frame_idx = 0;
    view->sprite_timer =
        lv_timer_create(sprite_timer_cb, s_sprite_anims[SPRITE_STATE_IDLE].period_ms, view);
    return view->root;
}

void home_view_apply(home_view_t *view, const home_present_model_t *model)
{
    if (view == NULL || model == NULL || view->root == NULL) {
        return;
    }

    lv_label_set_text(view->time_label, model->time_text);
    lv_label_set_text(view->date_label, model->date_line);

    lv_label_set_text_static(view->wifi_icon, model->wifi_symbol);
    lv_obj_set_style_text_color(view->wifi_icon, lv_color_hex(model->wifi_color), 0);
    set_status_dot_visible(view->wifi_dot, model->wifi_dot_visible);

    lv_obj_set_style_text_color(view->claude_icon, lv_color_hex(model->claude_color), 0);
    set_status_dot_visible(view->claude_dot, model->claude_dot_visible);

    lv_label_set_text_static(view->weather_icon, model->weather_symbol);
    lv_obj_set_style_text_color(view->weather_icon, lv_color_hex(model->weather_color), 0);
    lv_label_set_text(view->weather_label, model->weather_text);
    lv_obj_set_style_text_color(view->weather_label, lv_color_hex(model->weather_color), 0);

    refresh_sprite(view, model->sprite_state);
    refresh_bubble(view, model);
}

void home_view_set_hidden(home_view_t *view, bool hidden)
{
    lv_obj_t *objects[5];
    size_t i;

    if (view == NULL) {
        return;
    }

    objects[0] = view->status_bar;
    objects[1] = view->time_label;
    objects[2] = view->date_label;
    objects[3] = view->weather_row;
    objects[4] = view->sprite_img;

    for (i = 0; i < sizeof(objects) / sizeof(objects[0]); ++i) {
        if (objects[i] == NULL) {
            continue;
        }

        if (hidden) {
            lv_obj_add_flag(objects[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(objects[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (hidden && view->bubble_box != NULL) {
        lv_obj_add_flag(view->bubble_box, LV_OBJ_FLAG_HIDDEN);
    }
}

void home_view_resume(home_view_t *view)
{
    if (view != NULL && view->sprite_timer != NULL) {
        lv_timer_resume(view->sprite_timer);
    }
}

void home_view_suspend(home_view_t *view)
{
    if (view == NULL) {
        return;
    }

    if (view->sprite_timer != NULL) {
        lv_timer_pause(view->sprite_timer);
    }
    stop_bubble_timer(view);
}

void home_view_on_screensaver_enter(home_view_t *view)
{
    home_view_suspend(view);
    if (view != NULL && view->bubble_box != NULL) {
        lv_obj_add_flag(view->bubble_box, LV_OBJ_FLAG_HIDDEN);
    }
}

void home_view_on_screensaver_exit(home_view_t *view) { home_view_resume(view); }

void home_view_update_time(home_view_t *view, const char *time_text)
{
    if (view == NULL || view->time_label == NULL || time_text == NULL) {
        return;
    }

    lv_label_set_text(view->time_label, time_text);
}
