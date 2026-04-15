#ifndef APP_HOME_HOME_VIEW_H
#define APP_HOME_HOME_VIEW_H

#include <stdbool.h>
#include <stdint.h>

#include "home_presenter.h"
#include "lvgl.h"

typedef struct sprite_anim_def_t sprite_anim_def_t;

typedef struct {
    lv_obj_t *root;
    lv_obj_t *status_bar;
    lv_obj_t *time_label;
    lv_obj_t *date_label;
    lv_obj_t *wifi_item;
    lv_obj_t *wifi_icon;
    lv_obj_t *wifi_dot;
    lv_obj_t *settings_item;
    lv_obj_t *weather_row;
    lv_obj_t *weather_icon;
    lv_obj_t *weather_label;
    lv_obj_t *claude_item;
    lv_obj_t *claude_icon;
    lv_obj_t *claude_dot;
    lv_obj_t *sprite_img;
    lv_obj_t *bubble_box;
    lv_obj_t *bubble_label;
    sprite_state_t sprite_state;
    sprite_emotion_t sprite_emotion;
    const sprite_anim_def_t *sprite_anim;
    uint8_t frame_idx;
    lv_timer_t *sprite_timer;
    lv_timer_t *bubble_timer;
    bool bubble_dismissed;
} home_view_t;

lv_obj_t *home_view_create(home_view_t *view, lv_obj_t *parent);
void home_view_apply(home_view_t *view, const home_present_model_t *model);
void home_view_set_hidden(home_view_t *view, bool hidden);
void home_view_resume(home_view_t *view);
void home_view_suspend(home_view_t *view);
void home_view_on_screensaver_enter(home_view_t *view);
void home_view_on_screensaver_exit(home_view_t *view);
void home_view_update_time(home_view_t *view, const char *time_text);

#endif
