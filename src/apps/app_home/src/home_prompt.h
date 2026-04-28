#ifndef APP_HOME_HOME_PROMPT_H
#define APP_HOME_HOME_PROMPT_H

#include <stdbool.h>

#include "lvgl.h"

typedef struct {
    lv_obj_t *overlay;
    lv_obj_t *title_label;
    lv_obj_t *question_label;
    lv_obj_t *options_label;
    lv_obj_t *hint_label;
    lv_obj_t *btn_row;
    lv_obj_t *option_btns[4];
    uint8_t option_count;
} home_prompt_t;

void home_prompt_create(home_prompt_t *prompt, lv_obj_t *root);
void home_prompt_show_pending(home_prompt_t *prompt);
void home_prompt_hide(home_prompt_t *prompt);
void home_prompt_on_connection_changed(home_prompt_t *prompt, bool was_connected,
                                       bool is_connected);
bool home_prompt_is_visible(const home_prompt_t *prompt);

#endif
