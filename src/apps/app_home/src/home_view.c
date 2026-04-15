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

struct sprite_motion_def_t {
    uint16_t bob_period_ms;
    uint16_t bob_amplitude_px_x10;
};

static const sprite_anim_def_t s_sprite_anims[SPRITE_STATE_COUNT][SPRITE_EMOTION_COUNT] = {
    [SPRITE_STATE_IDLE] =
        {
            [SPRITE_EMOTION_NEUTRAL] = {sprite_idle_frames, SPRITE_IDLE_FRAME_COUNT, 333},
            [SPRITE_EMOTION_HAPPY] = {sprite_idle_happy_frames, SPRITE_IDLE_HAPPY_FRAME_COUNT, 333},
            [SPRITE_EMOTION_SAD] = {sprite_idle_sad_frames, SPRITE_IDLE_SAD_FRAME_COUNT, 333},
            [SPRITE_EMOTION_SOB] = {sprite_idle_sob_frames, SPRITE_IDLE_SOB_FRAME_COUNT, 333},
        },
    [SPRITE_STATE_WORKING] =
        {
            [SPRITE_EMOTION_NEUTRAL] = {sprite_working_frames, SPRITE_WORKING_FRAME_COUNT, 250},
            [SPRITE_EMOTION_HAPPY] =
                {
                    sprite_working_happy_frames,
                    SPRITE_WORKING_HAPPY_FRAME_COUNT,
                    250,
                },
            [SPRITE_EMOTION_SAD] = {sprite_working_sad_frames, SPRITE_WORKING_SAD_FRAME_COUNT, 250},
        },
    [SPRITE_STATE_WAITING] =
        {
            [SPRITE_EMOTION_NEUTRAL] = {sprite_waiting_frames, SPRITE_WAITING_FRAME_COUNT, 333},
            [SPRITE_EMOTION_HAPPY] =
                {
                    sprite_waiting_happy_frames,
                    SPRITE_WAITING_HAPPY_FRAME_COUNT,
                    333,
                },
            [SPRITE_EMOTION_SAD] = {sprite_waiting_sad_frames, SPRITE_WAITING_SAD_FRAME_COUNT, 333},
        },
    [SPRITE_STATE_COMPACTING] =
        {
            [SPRITE_EMOTION_NEUTRAL] =
                {
                    sprite_compacting_frames,
                    SPRITE_COMPACTING_FRAME_COUNT,
                    167,
                },
        },
    [SPRITE_STATE_SLEEPING] =
        {
            [SPRITE_EMOTION_NEUTRAL] = {sprite_sleeping_frames, SPRITE_SLEEPING_FRAME_COUNT, 500},
            [SPRITE_EMOTION_HAPPY] =
                {
                    sprite_sleeping_happy_frames,
                    SPRITE_SLEEPING_HAPPY_FRAME_COUNT,
                    500,
                },
        },
};

static const sprite_motion_def_t s_sprite_motions[SPRITE_STATE_COUNT][SPRITE_EMOTION_COUNT] = {
    [SPRITE_STATE_IDLE] =
        {
            [SPRITE_EMOTION_NEUTRAL] = {1500, 18},
            [SPRITE_EMOTION_HAPPY] = {1500, 20},
            [SPRITE_EMOTION_SAD] = {1500, 9},
            [SPRITE_EMOTION_SOB] = {0, 0},
        },
    [SPRITE_STATE_WORKING] =
        {
            [SPRITE_EMOTION_NEUTRAL] = {400, 8},
            [SPRITE_EMOTION_HAPPY] = {400, 10},
            [SPRITE_EMOTION_SAD] = {400, 4},
            [SPRITE_EMOTION_SOB] = {0, 0},
        },
    [SPRITE_STATE_WAITING] =
        {
            [SPRITE_EMOTION_NEUTRAL] = {1500, 10},
            [SPRITE_EMOTION_HAPPY] = {1500, 12},
            [SPRITE_EMOTION_SAD] = {1500, 5},
            [SPRITE_EMOTION_SOB] = {0, 0},
        },
    [SPRITE_STATE_COMPACTING] =
        {
            [SPRITE_EMOTION_NEUTRAL] = {0, 0},
            [SPRITE_EMOTION_HAPPY] = {0, 0},
            [SPRITE_EMOTION_SAD] = {0, 0},
            [SPRITE_EMOTION_SOB] = {0, 0},
        },
    [SPRITE_STATE_SLEEPING] =
        {
            [SPRITE_EMOTION_NEUTRAL] = {0, 0},
            [SPRITE_EMOTION_HAPPY] = {0, 0},
            [SPRITE_EMOTION_SAD] = {0, 0},
            [SPRITE_EMOTION_SOB] = {0, 0},
        },
};

static const sprite_anim_def_t *current_anim(const home_view_t *view) { return view->sprite_anim; }
static const sprite_motion_def_t *current_motion(const home_view_t *view)
{
    return view->sprite_motion;
}

static bool anim_ready(const sprite_anim_def_t *anim)
{
    return anim != NULL && anim->frames != NULL && anim->num_frames != 0U;
}

static bool motion_ready(const sprite_motion_def_t *motion)
{
    return motion != NULL && motion->bob_period_ms != 0U && motion->bob_amplitude_px_x10 != 0U;
}

static const sprite_anim_def_t *fallback_anim(void)
{
    const sprite_anim_def_t *anim = &s_sprite_anims[SPRITE_STATE_IDLE][SPRITE_EMOTION_NEUTRAL];
    return anim_ready(anim) ? anim : NULL;
}

static const sprite_motion_def_t *lookup_motion(sprite_state_t state, sprite_emotion_t emotion)
{
    if ((unsigned)state >= SPRITE_STATE_COUNT) {
        state = SPRITE_STATE_IDLE;
    }
    if ((unsigned)emotion >= SPRITE_EMOTION_COUNT) {
        emotion = SPRITE_EMOTION_NEUTRAL;
    }

    return &s_sprite_motions[state][emotion];
}

static const sprite_anim_def_t *lookup_anim(sprite_state_t state, sprite_emotion_t emotion,
                                            sprite_emotion_t *resolved_emotion)
{
    const sprite_anim_def_t *anim;

    if ((unsigned)state >= SPRITE_STATE_COUNT) {
        state = SPRITE_STATE_IDLE;
    }
    if ((unsigned)emotion >= SPRITE_EMOTION_COUNT) {
        emotion = SPRITE_EMOTION_NEUTRAL;
    }

    anim = &s_sprite_anims[state][emotion];
    if (anim_ready(anim)) {
        if (resolved_emotion != NULL) {
            *resolved_emotion = emotion;
        }
        return anim;
    }

    if (emotion == SPRITE_EMOTION_SOB) {
        anim = &s_sprite_anims[state][SPRITE_EMOTION_SAD];
        if (anim_ready(anim)) {
            if (resolved_emotion != NULL) {
                *resolved_emotion = SPRITE_EMOTION_SAD;
            }
            return anim;
        }
    }

    anim = &s_sprite_anims[state][SPRITE_EMOTION_NEUTRAL];
    if (anim_ready(anim)) {
        if (resolved_emotion != NULL) {
            *resolved_emotion = SPRITE_EMOTION_NEUTRAL;
        }
        return anim;
    }

    if (resolved_emotion != NULL) {
        *resolved_emotion = SPRITE_EMOTION_NEUTRAL;
    }
    return fallback_anim();
}

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

static int32_t wave_offset_x10(uint32_t elapsed_ms, uint16_t period_ms, int32_t amplitude_x10)
{
    uint32_t phase_ms;
    int32_t angle_deg;

    if (period_ms == 0U || amplitude_x10 == 0) {
        return 0;
    }

    phase_ms = elapsed_ms % period_ms;
    angle_deg = (int32_t)(((uint64_t)phase_ms * 360ULL) / period_ms);
    return (lv_trigo_sin((uint16_t)angle_deg) * amplitude_x10) / LV_TRIGO_SIN_MAX;
}

static lv_coord_t round_x10_to_coord(int32_t value_x10)
{
    return (lv_coord_t)((value_x10 >= 0) ? ((value_x10 + 5) / 10) : ((value_x10 - 5) / 10));
}

static void apply_sprite_transform(home_view_t *view, lv_coord_t motion_x, lv_coord_t motion_y)
{
    if (view == NULL || view->sprite_img == NULL) {
        return;
    }

    view->sprite_motion_x = motion_x;
    view->sprite_motion_y = motion_y;
    lv_obj_set_style_translate_x(view->sprite_img, view->sprite_base_translate_x + motion_x, 0);
    lv_obj_set_style_translate_y(view->sprite_img, view->sprite_base_translate_y + motion_y, 0);
}

static void reset_motion_phase(home_view_t *view)
{
    if (view == NULL) {
        return;
    }

    view->motion_tick_ms = 0;
    apply_sprite_transform(view, 0, 0);
}

static void sync_motion_timer(home_view_t *view)
{
    if (view == NULL || view->motion_timer == NULL) {
        return;
    }

    if (motion_ready(current_motion(view))) {
        lv_timer_resume(view->motion_timer);
    } else {
        lv_timer_pause(view->motion_timer);
    }
}

static void motion_timer_cb(lv_timer_t *timer)
{
    home_view_t *view = lv_timer_get_user_data(timer);
    const sprite_motion_def_t *motion;
    int32_t bob_offset_y_x10;

    if (view == NULL || view->sprite_img == NULL) {
        return;
    }

    motion = current_motion(view);
    if (!motion_ready(motion)) {
        apply_sprite_transform(view, 0, 0);
        return;
    }

    view->motion_tick_ms += HOME_SPRITE_MOTION_PERIOD_MS;
    bob_offset_y_x10 =
        wave_offset_x10(view->motion_tick_ms, motion->bob_period_ms, motion->bob_amplitude_px_x10);
    apply_sprite_transform(view, 0, round_x10_to_coord(bob_offset_y_x10));
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

static void refresh_sprite(home_view_t *view, sprite_state_t new_state,
                           sprite_emotion_t new_emotion)
{
    const sprite_anim_def_t *anim;
    const sprite_motion_def_t *motion;
    sprite_emotion_t resolved_emotion = new_emotion;

    if (view->sprite_img == NULL) {
        return;
    }

    if ((unsigned)new_state >= SPRITE_STATE_COUNT) {
        new_state = SPRITE_STATE_IDLE;
    }
    if ((unsigned)new_emotion >= SPRITE_EMOTION_COUNT) {
        new_emotion = SPRITE_EMOTION_NEUTRAL;
    }

    anim = lookup_anim(new_state, new_emotion, &resolved_emotion);
    motion = lookup_motion(new_state, new_emotion);
    if (new_state == view->sprite_state && new_emotion == view->sprite_emotion &&
        resolved_emotion == view->sprite_display_emotion && current_anim(view) == anim &&
        current_motion(view) == motion) {
        return;
    }
    if (!anim_ready(anim)) {
        return;
    }

    view->sprite_state = new_state;
    view->sprite_emotion = new_emotion;
    view->sprite_display_emotion = resolved_emotion;
    view->sprite_anim = anim;
    view->sprite_motion = motion;
    view->frame_idx = 0;
    lv_image_set_src(view->sprite_img, &anim->frames[0]);
    if (view->sprite_timer != NULL) {
        lv_timer_set_period(view->sprite_timer, anim->period_ms);
    }
    reset_motion_phase(view);
    sync_motion_timer(view);
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
    lv_obj_set_flex_align(view->weather_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(view->weather_row, HOME_WEATHER_ICON_GAP, 0);

    view->weather_icon = lv_label_create(view->weather_row);
    lv_label_set_text_static(view->weather_icon, APP_HOME_SYMBOL_WEATHER_UNKNOWN);
    lv_obj_set_style_text_font(view->weather_icon, &app_home_status_font, 0);

    view->weather_label = lv_label_create(view->weather_row);
    lv_obj_set_width(view->weather_label,
                     HOME_LEFT_HALF_W - HOME_STATUS_ITEM_BOX_SIZE - HOME_WEATHER_ICON_GAP);
    lv_label_set_long_mode(view->weather_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(view->weather_label, ui_font_text_11(), 0);
    lv_obj_set_style_text_color(view->weather_label, lv_color_hex(HOME_WEATHER_TEXT_COLOR), 0);

    view->sprite_img = lv_image_create(view->root);
    lv_image_set_src(view->sprite_img, &sprite_idle_frames[0]);
    lv_image_set_scale(view->sprite_img, HOME_SPRITE_SCALE);
    lv_obj_set_style_image_recolor(view->sprite_img, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(view->sprite_img, LV_OPA_TRANSP, 0);
    view->sprite_base_x = HOME_SPRITE_X_OFFSET;
    view->sprite_base_y = HOME_SPRITE_Y_OFFSET;
    view->sprite_base_translate_x = 0;
    view->sprite_base_translate_y = 0;
    lv_obj_align(view->sprite_img, LV_ALIGN_RIGHT_MID, view->sprite_base_x, view->sprite_base_y);

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
    view->sprite_emotion = SPRITE_EMOTION_NEUTRAL;
    view->sprite_display_emotion = SPRITE_EMOTION_NEUTRAL;
    view->sprite_anim = &s_sprite_anims[SPRITE_STATE_IDLE][SPRITE_EMOTION_NEUTRAL];
    view->sprite_motion = &s_sprite_motions[SPRITE_STATE_IDLE][SPRITE_EMOTION_NEUTRAL];
    view->frame_idx = 0;
    reset_motion_phase(view);
    view->sprite_timer = lv_timer_create(
        sprite_timer_cb, s_sprite_anims[SPRITE_STATE_IDLE][SPRITE_EMOTION_NEUTRAL].period_ms, view);
    view->motion_timer = lv_timer_create(motion_timer_cb, HOME_SPRITE_MOTION_PERIOD_MS, view);
    sync_motion_timer(view);
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

    refresh_sprite(view, model->sprite_state, model->sprite_emotion);
    refresh_bubble(view, model);
}

void home_view_set_hidden(home_view_t *view, bool hidden)
{
    lv_obj_t *objects[4];
    size_t i;

    if (view == NULL) {
        return;
    }

    /* The full-screen screensaver overlay already covers the sprite, so keep this helper focused
     * on the standalone Home chrome instead of toggling the image object here as well. */
    objects[0] = view->status_bar;
    objects[1] = view->time_label;
    objects[2] = view->date_label;
    objects[3] = view->weather_row;

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
    if (view == NULL) {
        return;
    }

    if (view->sprite_timer != NULL) {
        lv_timer_resume(view->sprite_timer);
    }
    if (view->motion_timer != NULL && motion_ready(current_motion(view))) {
        lv_timer_resume(view->motion_timer);
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
    if (view->motion_timer != NULL) {
        lv_timer_pause(view->motion_timer);
    }
    reset_motion_phase(view);
    stop_bubble_timer(view);
}

void home_view_on_screensaver_enter(home_view_t *view)
{
    home_view_suspend(view);
    if (view != NULL && view->bubble_box != NULL) {
        lv_obj_add_flag(view->bubble_box, LV_OBJ_FLAG_HIDDEN);
    }
}

void home_view_on_screensaver_exit(home_view_t *view)
{
    reset_motion_phase(view);
    home_view_resume(view);
}

void home_view_update_time(home_view_t *view, const char *time_text)
{
    if (view == NULL || view->time_label == NULL || time_text == NULL) {
        return;
    }

    lv_label_set_text(view->time_label, time_text);
}
