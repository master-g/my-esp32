#ifndef APP_HOME_HOME_APPROVAL_H
#define APP_HOME_HOME_APPROVAL_H

#include <stdbool.h>

#include "lvgl.h"

/* Read-only "Claude is waiting" status overlay. The device never returns a
 * decision; this only displays the coarse interaction type for every waiting
 * type (tool permission, Elicitation, AskUserQuestion). */
typedef struct {
    lv_obj_t *overlay;
    lv_obj_t *status_label;
    lv_obj_t *type_label;
    lv_timer_t *timeout_timer;
} home_approval_t;

void home_approval_create(home_approval_t *approval, lv_obj_t *root);
void home_approval_show_pending(home_approval_t *approval);
void home_approval_hide(home_approval_t *approval);
void home_approval_on_connection_changed(home_approval_t *approval, bool was_connected,
                                         bool is_connected);
bool home_approval_is_visible(const home_approval_t *approval);

#endif
