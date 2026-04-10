#include "app_notify.h"

#include "bsp_board_config.h"
#include "lvgl.h"
#include "service_claude.h"
#include "ui_fonts.h"

#define APP_NOTIFY_BODY_Y 44

static lv_obj_t *s_root;
static lv_obj_t *s_status;

static esp_err_t app_notify_init(void) { return ESP_OK; }

static lv_obj_t *app_notify_create_root(lv_obj_t *parent)
{
    lv_obj_t *title = NULL;

    s_root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(0x13101d), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_root, 16, 0);

    title = lv_label_create(s_root);
    lv_label_set_text(title, "Notify");
    lv_obj_set_style_text_font(title, ui_font_text_22(), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xf3e8ff), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_status = lv_label_create(s_root);
    lv_label_set_text(s_status, "Claude bridge pending");
    lv_obj_set_width(s_status, BSP_LCD_H_RES - 32);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_status, ui_font_text_11(), 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(0xd5cbe8), 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 0, APP_NOTIFY_BODY_Y);
    return s_root;
}

static void app_notify_resume(void)
{
    claude_service_start();
    if (s_status != NULL) {
        lv_label_set_text(s_status, "Claude bridge connecting...");
    }
}

static void app_notify_suspend(void) { claude_service_stop(); }

static void app_notify_handle_event(const app_event_t *event)
{
    if (event == NULL || s_status == NULL) {
        return;
    }

    if (event->type == APP_EVENT_DATA_CLAUDE || event->type == APP_EVENT_ENTER) {
        claude_snapshot_t snapshot;
        claude_service_get_snapshot(&snapshot);
        lv_label_set_text_fmt(s_status, "state=%d\nunread=%s\n%s", snapshot.conn_state,
                              snapshot.unread ? "yes" : "no", snapshot.title);
    }
}

const app_descriptor_t *app_notify_get_descriptor(void)
{
    static const app_descriptor_t descriptor = {
        .id = APP_ID_NOTIFY,
        .name = "Notify",
        .init = app_notify_init,
        .create_root = app_notify_create_root,
        .resume = app_notify_resume,
        .suspend = app_notify_suspend,
        .handle_event = app_notify_handle_event,
    };

    return &descriptor;
}
