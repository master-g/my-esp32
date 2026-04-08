#include "app_home.h"

#include <stdio.h>
#include <string.h>

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
} app_home_view_t;

static app_home_view_t s_view;

static void display_city_name(char *dst, size_t dst_size, const char *src)
{
    size_t len = 0;

    if (dst == NULL || dst_size == 0) {
        return;
    }
    if (src == NULL || src[0] == '\0') {
        snprintf(dst, dst_size, "%s", "--");
        return;
    }

    while (src[len] != '\0' && src[len] != ',' && len + 1 < dst_size) {
        dst[len] = src[len];
        len++;
    }
    while (len > 0 && dst[len - 1] == ' ') {
        len--;
    }
    dst[len] = '\0';
}

static int rounded_temperature_c(int16_t temperature_c_tenths)
{
    if (temperature_c_tenths >= 0) {
        return (temperature_c_tenths + 5) / 10;
    }

    return (temperature_c_tenths - 5) / 10;
}

static void refresh_view(void)
{
    const home_snapshot_t *snapshot = home_service_get_snapshot();
    char weather_line[96];
    char city_name[24];
    char claude_line[24];
    char network_line[24];
    char date_line[48];

    if (s_view.root == NULL) {
        return;
    }

    display_city_name(city_name, sizeof(city_name), snapshot->city_text);
    snprintf(weather_line, sizeof(weather_line), "%s  %s  %d°C", city_name,
             snapshot->weather_text[0] ? snapshot->weather_text : "--",
             rounded_temperature_c(snapshot->temperature_c_tenths));
    if (snapshot->weather_stale) {
        snprintf(weather_line + strlen(weather_line), sizeof(weather_line) - strlen(weather_line),
                 "  cached");
    }
    snprintf(claude_line, sizeof(claude_line), "%s",
             snapshot->claude_unread ? "Claude new" : "Claude ok");
    snprintf(network_line, sizeof(network_line), "%s",
             snapshot->wifi_connected ? "WiFi online" : "WiFi offline");
    snprintf(date_line, sizeof(date_line), "%s  %s", snapshot->date_text, snapshot->weekday_text);

    lv_label_set_text(s_view.time_label, snapshot->time_text);
    lv_label_set_text(s_view.date_label, date_line);
    lv_label_set_text(s_view.network_label, network_line);
    lv_label_set_text(s_view.weather_label, weather_line);
    lv_label_set_text(s_view.claude_label, claude_line);
}

static esp_err_t app_home_init(void)
{
    home_service_refresh_snapshot();
    return ESP_OK;
}

static lv_obj_t *app_home_create_root(lv_obj_t *parent)
{
    lv_obj_t *root = lv_obj_create(parent);

    s_view.root = root;
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x0f1418), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(root, 16, 0);

    s_view.time_label = lv_label_create(root);
    lv_obj_set_style_text_font(s_view.time_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_view.time_label, lv_color_hex(0xa6f0ff), 0);
    lv_obj_align(s_view.time_label, LV_ALIGN_TOP_LEFT, 0, 28);

    s_view.date_label = lv_label_create(root);
    lv_obj_set_style_text_font(s_view.date_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_view.date_label, lv_color_hex(0xb7c4cc), 0);
    lv_obj_align(s_view.date_label, LV_ALIGN_TOP_LEFT, 2, 92);

    s_view.network_label = lv_label_create(root);
    lv_obj_set_style_text_font(s_view.network_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_view.network_label, lv_color_hex(0x8da0ab), 0);
    lv_obj_align(s_view.network_label, LV_ALIGN_TOP_RIGHT, 0, 2);

    s_view.weather_label = lv_label_create(root);
    lv_obj_set_width(s_view.weather_label, BSP_LCD_H_RES - 32);
    lv_label_set_long_mode(s_view.weather_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_view.weather_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_view.weather_label, lv_color_hex(0xe3edf2), 0);
    lv_obj_align(s_view.weather_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    s_view.claude_label = lv_label_create(root);
    lv_obj_set_style_text_font(s_view.claude_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_view.claude_label, lv_color_hex(0x8da0ab), 0);
    lv_obj_align(s_view.claude_label, LV_ALIGN_TOP_RIGHT, 0, 18);

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
