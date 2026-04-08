#include "app_home.h"

#include <stdio.h>

#include "bsp_board.h"
#include "bsp_board_config.h"
#include "lvgl.h"
#include "service_home.h"

typedef struct {
    lv_obj_t *root;
    lv_obj_t *time_label;
    lv_obj_t *date_label;
    lv_obj_t *network_label;
    lv_obj_t *weather_label;
    lv_obj_t *claude_label;
    lv_obj_t *touch_label;
    uint32_t touch_count;
} app_home_view_t;

static app_home_view_t s_view;

static void refresh_view(void)
{
    const home_snapshot_t *snapshot = home_service_get_snapshot();
    char weather_line[64];
    char claude_line[32];
    char network_line[48];
    char date_line[40];

    if (s_view.root == NULL) {
        return;
    }

    snprintf(weather_line,
             sizeof(weather_line),
             "%s  %s  %d.%dC",
             snapshot->city_text[0] ? snapshot->city_text : "--",
             snapshot->weather_text[0] ? snapshot->weather_text : "--",
             snapshot->temperature_c_tenths / 10,
             snapshot->temperature_c_tenths >= 0 ? snapshot->temperature_c_tenths % 10 : -(snapshot->temperature_c_tenths % 10));
    snprintf(claude_line, sizeof(claude_line), "Claude unread: %s", snapshot->claude_unread ? "yes" : "no");
    snprintf(network_line,
             sizeof(network_line),
             "Network: %s | %s",
             snapshot->wifi_connected ? "connected" : "offline",
             snapshot->ntp_synced ? "SNTP" : (snapshot->rtc_valid ? "RTC" : "unsynced"));
    snprintf(date_line, sizeof(date_line), "%s  %s", snapshot->date_text, snapshot->weekday_text);

    lv_label_set_text(s_view.time_label, snapshot->time_hhmm);
    lv_label_set_text(s_view.date_label, date_line);
    lv_label_set_text(s_view.network_label, network_line);
    lv_label_set_text(s_view.weather_label, weather_line);
    lv_label_set_text(s_view.claude_label, claude_line);
}

static void home_touch_button_cb(lv_event_t *event)
{
    char text[48];
    (void)event;
    s_view.touch_count++;
    snprintf(text, sizeof(text), "Touch path OK (%lu)", (unsigned long)s_view.touch_count);
    lv_label_set_text(s_view.touch_label, text);
}

static esp_err_t app_home_init(void)
{
    home_service_refresh_snapshot();
    return ESP_OK;
}

static lv_obj_t *app_home_create_root(lv_obj_t *parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_t *title = NULL;
    lv_obj_t *button = NULL;

    s_view.root = root;
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x11161c), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(root, 16, 0);

    title = lv_label_create(root);
    lv_label_set_text(title, "ESP32 Dashboard");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xf2f6f8), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_view.time_label = lv_label_create(root);
    lv_obj_set_style_text_font(s_view.time_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_view.time_label, lv_color_hex(0x8ee3ff), 0);
    lv_obj_align(s_view.time_label, LV_ALIGN_TOP_LEFT, 0, 46);

    s_view.date_label = lv_label_create(root);
    lv_obj_set_style_text_color(s_view.date_label, lv_color_hex(0xc3ced6), 0);
    lv_obj_align(s_view.date_label, LV_ALIGN_TOP_LEFT, 0, 80);

    s_view.network_label = lv_label_create(root);
    lv_obj_set_width(s_view.network_label, 280);
    lv_label_set_long_mode(s_view.network_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_view.network_label, lv_color_hex(0xc3ced6), 0);
    lv_obj_align(s_view.network_label, LV_ALIGN_TOP_LEFT, 320, 46);

    s_view.weather_label = lv_label_create(root);
    lv_obj_set_width(s_view.weather_label, 280);
    lv_label_set_long_mode(s_view.weather_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_view.weather_label, lv_color_hex(0xf2f6f8), 0);
    lv_obj_align(s_view.weather_label, LV_ALIGN_TOP_LEFT, 0, 112);

    s_view.claude_label = lv_label_create(root);
    lv_obj_set_width(s_view.claude_label, 280);
    lv_label_set_long_mode(s_view.claude_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_view.claude_label, lv_color_hex(0xc3ced6), 0);
    lv_obj_align(s_view.claude_label, LV_ALIGN_TOP_LEFT, 320, 92);

    button = lv_button_create(root);
    lv_obj_set_size(button, 220, 44);
    lv_obj_align(button, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x1f8fff), 0);
    lv_obj_add_event_cb(button, home_touch_button_cb, LV_EVENT_CLICKED, NULL);

    s_view.touch_label = lv_label_create(button);
    lv_label_set_text(s_view.touch_label, "Touch path OK?");
    lv_obj_center(s_view.touch_label);

    refresh_view();
    return root;
}

static void app_home_resume(void)
{
    home_service_refresh_snapshot();
    if (!bsp_board_lock(UINT32_MAX)) {
        return;
    }
    refresh_view();
    bsp_board_unlock();
}

static void app_home_handle_event(const app_event_t *event)
{
    if (event == NULL) {
        return;
    }

    switch (event->type) {
    case APP_EVENT_POWER_CHANGED:
    case APP_EVENT_NET_CHANGED:
    case APP_EVENT_DATA_CLAUDE:
    case APP_EVENT_DATA_WEATHER:
    case APP_EVENT_TICK_1S:
        home_service_refresh_snapshot();
        refresh_view();
        break;
    default:
        break;
    }
}

const app_descriptor_t *app_home_get_descriptor(void)
{
    static const app_descriptor_t descriptor = {
        .id = APP_ID_HOME,
        .name = "Home",
        .init = app_home_init,
        .create_root = app_home_create_root,
        .resume = app_home_resume,
        .suspend = NULL,
        .handle_event = app_home_handle_event,
    };

    return &descriptor;
}
