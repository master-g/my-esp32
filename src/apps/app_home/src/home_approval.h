#ifndef APP_HOME_HOME_APPROVAL_H
#define APP_HOME_HOME_APPROVAL_H

#include <stdbool.h>

#include "lvgl.h"

/* Read-only "Claude is waiting" alert lifecycle. The device never returns a
 * decision; it surfaces only the coarse interaction type for every waiting type
 * (tool permission, Elicitation, AskUserQuestion). The waiting state is drawn by
 * the Home sprite + bubble (driven by the snapshot); this struct only keeps the
 * screen lit and runs a self-timeout backstop while a request is outstanding. */
typedef struct {
    bool active;
    lv_timer_t *timeout_timer;
} home_approval_t;

void home_approval_create(home_approval_t *approval, lv_obj_t *root);
void home_approval_show_pending(home_approval_t *approval);
void home_approval_hide(home_approval_t *approval);
void home_approval_on_connection_changed(home_approval_t *approval, bool was_connected,
                                         bool is_connected);
bool home_approval_is_visible(const home_approval_t *approval);

#endif
