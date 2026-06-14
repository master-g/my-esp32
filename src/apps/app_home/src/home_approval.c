#include "home_approval.h"

#include <stdint.h>
#include <string.h>

#include "bsp_board_config.h"
#include "device_link.h"
#include "home_internal.h"
#include "ui_theme.h"
#include "ui_fonts.h"

/* Device-side fallback so a wedged-but-connected host cannot leave the overlay
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

static void approval_timeout_cb(lv_timer_t *timer)
{
    home_approval_t *approval = lv_timer_get_user_data(timer);

    if (approval == NULL) {
        return;
    }
    /* One-shot timer auto-deletes after this callback returns; drop our handle
     * first so home_approval_hide() does not delete it a second time. */
    approval->timeout_timer = NULL;
    device_link_dismiss_approval();
    device_link_dismiss_prompt();
    if (approval->overlay != NULL) {
        lv_obj_add_flag(approval->overlay, LV_OBJ_FLAG_HIDDEN);
    }
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

    /* Read-only: a fixed status line plus the coarse interaction type. No buttons,
     * no touch handlers — the device cannot resolve the request. */
    approval->status_label = lv_label_create(approval->overlay);
    lv_obj_set_style_text_font(approval->status_label, ui_font_text_11(), 0);
    lv_obj_set_style_text_color(approval->status_label, ui_theme_color(UI_THEME_COLOR_TEXT_MUTED),
                                0);
    lv_label_set_text(approval->status_label, "Claude is waiting");
    lv_obj_align(approval->status_label, LV_ALIGN_LEFT_MID, 0, -12);

    approval->type_label = lv_label_create(approval->overlay);
    lv_obj_set_style_text_font(approval->type_label, ui_font_text_11(), 0);
    lv_obj_set_style_text_color(approval->type_label, ui_theme_color(UI_THEME_COLOR_TEXT_PRIMARY),
                                0);
    lv_label_set_long_mode(approval->type_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(approval->type_label, BSP_LCD_H_RES - 32);
    lv_label_set_text(approval->type_label, "");
    lv_obj_align(approval->type_label, LV_ALIGN_LEFT_MID, 0, 8);
}

void home_approval_show_pending(home_approval_t *approval)
{
    approval_request_t areq;
    prompt_request_t preq;
    const char *type = NULL;

    if (approval == NULL || approval->overlay == NULL) {
        return;
    }

    /* Approvals win the single overlay; fall back to a pending prompt. Both show
     * only a coarse type, never command/argument/option content. */
    if (device_link_get_pending_approval(&areq)) {
        type = areq.type_label;
    } else if (device_link_get_pending_prompt(&preq)) {
        type = preq.type_label;
    } else {
        return;
    }

    lv_label_set_text(approval->type_label, (type != NULL && type[0] != '\0') ? type : "Request");
    lv_obj_clear_flag(approval->overlay, LV_OBJ_FLAG_HIDDEN);
    approval_arm_timeout(approval);
}

void home_approval_hide(home_approval_t *approval)
{
    if (approval == NULL || approval->overlay == NULL) {
        return;
    }
    approval_clear_timeout(approval);
    lv_obj_add_flag(approval->overlay, LV_OBJ_FLAG_HIDDEN);
}

void home_approval_on_connection_changed(home_approval_t *approval, bool was_connected,
                                         bool is_connected)
{
    if (approval == NULL || !was_connected || is_connected || !home_approval_is_visible(approval)) {
        return;
    }

    /* Connection lost: clear the overlay without returning any decision. */
    device_link_dismiss_approval();
    device_link_dismiss_prompt();
    home_approval_hide(approval);
}

bool home_approval_is_visible(const home_approval_t *approval)
{
    return approval != NULL && approval->overlay != NULL &&
           !lv_obj_has_flag(approval->overlay, LV_OBJ_FLAG_HIDDEN);
}
