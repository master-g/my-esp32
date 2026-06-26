#include "home_approval.h"

#include <string.h>

#include "device_link.h"
#include "lvgl.h"
#include "system_state.h"

/* Device-side fallback so a wedged-but-connected host cannot leave the alert
 * stuck. The host drives normal dismissal (PostToolUse/Stop + host timeout); this
 * is the device's own backstop, which the removed decision RPC used to provide. */
#define HOME_APPROVAL_SELF_TIMEOUT_MS (5 * 60 * 1000)

static void approval_clear_timeout(home_approval_t *approval)
{
    if (approval->timeout_timer != NULL) {
        lv_timer_delete(approval->timeout_timer);
        approval->timeout_timer = NULL;
    }
}

/* Single hide path for every dismissal reason (explicit hide, self-timeout,
 * connection loss). The waiting state is drawn by the Home sprite + bubble; this
 * module only keeps the screen lit while a request is outstanding, so tearing
 * down just clears the alert flag and the backstop timer. */
static void approval_teardown(home_approval_t *approval)
{
    approval_clear_timeout(approval);
    approval->active = false;
    system_state_set_alert_active(false); /* power_runtime returns to idle dimming */
}

static void approval_timeout_cb(lv_timer_t *timer)
{
    home_approval_t *approval = lv_timer_get_user_data(timer);

    if (approval == NULL) {
        return;
    }
    /* One-shot timer auto-deletes after this callback returns; drop our handle
     * first so approval_teardown() does not delete it a second time. */
    approval->timeout_timer = NULL;
    device_link_dismiss_approval();
    device_link_dismiss_prompt();
    approval_teardown(approval);
}

static void approval_arm_timeout(home_approval_t *approval)
{
    if (approval->timeout_timer != NULL) {
        lv_timer_reset(approval->timeout_timer);
        return;
    }
    approval->timeout_timer =
        lv_timer_create(approval_timeout_cb, HOME_APPROVAL_SELF_TIMEOUT_MS, approval);
    if (approval->timeout_timer != NULL) {
        lv_timer_set_repeat_count(approval->timeout_timer, 1);
    }
}

void home_approval_create(home_approval_t *approval, lv_obj_t *root)
{
    (void)root; /* the waiting state renders as the Home sprite + bubble */

    if (approval == NULL) {
        return;
    }

    memset(approval, 0, sizeof(*approval));
}

void home_approval_show_pending(home_approval_t *approval)
{
    if (approval == NULL) {
        return;
    }

    /* Only assert the alert if something is actually pending. The waiting state
     * itself (sprite + bubble) is driven by the Home snapshot; this module's job
     * is to keep the screen lit and run the self-timeout backstop while a request
     * is outstanding. At most one interaction is pending (device_link makes the
     * approval and prompt slots mutually exclusive). */
    if (!device_link_get_pending_approval(NULL) && !device_link_get_pending_prompt(NULL)) {
        return;
    }
    if (!approval->active) {
        approval->active = true;
        system_state_set_alert_active(true); /* keep the screen lit while waiting */
    }
    approval_arm_timeout(approval);
}

void home_approval_hide(home_approval_t *approval)
{
    if (approval == NULL) {
        return;
    }
    approval_teardown(approval);
}

void home_approval_on_connection_changed(home_approval_t *approval, bool was_connected,
                                         bool is_connected)
{
    if (approval == NULL || !was_connected || is_connected || !home_approval_is_visible(approval)) {
        return;
    }

    /* Connection lost: clear the alert without returning any decision. */
    device_link_dismiss_approval();
    device_link_dismiss_prompt();
    approval_teardown(approval);
}

bool home_approval_is_visible(const home_approval_t *approval)
{
    return approval != NULL && approval->active;
}
