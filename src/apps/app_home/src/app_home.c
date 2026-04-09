#include "app_home.h"

#include <stdio.h>
#include <string.h>

#include "bsp_board.h"
#include "bsp_board_config.h"
#include "generated/app_home_status_font.h"
#include "lvgl.h"
#include "service_home.h"

#define HOME_STATUS_BAR_HEIGHT 20
#define HOME_STATUS_BAR_WIDTH 80
#define HOME_STATUS_ICON_GAP 12
#define HOME_STATUS_ICON_SIZE 16
#define HOME_STATUS_ITEM_BOX_SIZE (HOME_STATUS_ICON_SIZE + 4)
#define HOME_STATUS_BAR_Y 4
#define HOME_TIME_Y 30
#define HOME_WIFI_ONLINE_COLOR 0xa6f0ff
#define HOME_WIFI_CONNECTING_COLOR 0xf3c76d
#define HOME_ICON_MUTED_COLOR 0x5f6b72
#define HOME_CLAUDE_ACTIVE_COLOR 0xe9e0cf
#define HOME_CLAUDE_UNREAD_DOT_COLOR 0xf06c41
#define HOME_WIFI_CONNECTING_DOT_COLOR 0xf3c76d
#define HOME_WEATHER_ROW_HEIGHT 20
#define HOME_WEATHER_ICON_GAP 8
#define HOME_WEATHER_ICON_Y_OFFSET 0
#define HOME_WEATHER_TEXT_Y_OFFSET 1
#define HOME_WEATHER_TEXT_COLOR 0xe3edf2
#define HOME_WEATHER_MUTED_COLOR 0x8da0ab

typedef struct {
    lv_obj_t *root;
    lv_obj_t *status_bar;
    lv_obj_t *time_label;
    lv_obj_t *date_label;
    lv_obj_t *wifi_item;
    lv_obj_t *wifi_icon;
    lv_obj_t *wifi_dot;
    lv_obj_t *weather_row;
    lv_obj_t *weather_icon;
    lv_obj_t *weather_label;
    lv_obj_t *claude_item;
    lv_obj_t *claude_icon;
    lv_obj_t *claude_dot;
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

static bool home_snapshot_has_weather(const home_snapshot_t *snapshot)
{
    return snapshot != NULL && snapshot->weather_available && snapshot->updated_at_epoch_s != 0;
}

static const char *weather_icon_symbol(weather_icon_t icon)
{
    switch (icon) {
    case WEATHER_ICON_CLEAR_DAY:
        return APP_HOME_SYMBOL_WEATHER_CLEAR_DAY;
    case WEATHER_ICON_CLEAR_NIGHT:
        return APP_HOME_SYMBOL_WEATHER_CLEAR_NIGHT;
    case WEATHER_ICON_PARTLY_CLOUDY_DAY:
        return APP_HOME_SYMBOL_WEATHER_PARTLY_CLOUDY_DAY;
    case WEATHER_ICON_PARTLY_CLOUDY_NIGHT:
        return APP_HOME_SYMBOL_WEATHER_PARTLY_CLOUDY_NIGHT;
    case WEATHER_ICON_CLOUDY:
        return APP_HOME_SYMBOL_WEATHER_CLOUDY;
    case WEATHER_ICON_FOG:
        return APP_HOME_SYMBOL_WEATHER_FOG;
    case WEATHER_ICON_DRIZZLE:
        return APP_HOME_SYMBOL_WEATHER_DRIZZLE;
    case WEATHER_ICON_RAIN:
        return APP_HOME_SYMBOL_WEATHER_RAIN;
    case WEATHER_ICON_HEAVY_RAIN:
        return APP_HOME_SYMBOL_WEATHER_HEAVY_RAIN;
    case WEATHER_ICON_SNOW:
        return APP_HOME_SYMBOL_WEATHER_SNOW;
    case WEATHER_ICON_THUNDER:
        return APP_HOME_SYMBOL_WEATHER_THUNDER;
    case WEATHER_ICON_UNKNOWN:
    default:
        return APP_HOME_SYMBOL_WEATHER_UNKNOWN;
    }
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

static void refresh_status_bar(const home_snapshot_t *snapshot)
{
    lv_color_t wifi_color = lv_color_hex(HOME_ICON_MUTED_COLOR);
    lv_color_t claude_color = lv_color_hex(HOME_ICON_MUTED_COLOR);

    if (snapshot->wifi_connected) {
        wifi_color = lv_color_hex(HOME_WIFI_ONLINE_COLOR);
    } else if (snapshot->wifi_connecting) {
        wifi_color = lv_color_hex(HOME_WIFI_CONNECTING_COLOR);
    }

    if (snapshot->claude_unread) {
        claude_color = lv_color_hex(HOME_CLAUDE_ACTIVE_COLOR);
    } else if (snapshot->claude_connected) {
        claude_color = lv_color_hex(0xaebcc4);
    }

    lv_obj_set_style_text_color(s_view.wifi_icon, wifi_color, 0);
    lv_obj_set_style_text_color(s_view.claude_icon, claude_color, 0);
    set_status_dot_visible(s_view.wifi_dot, snapshot->wifi_connecting && !snapshot->wifi_connected);
    set_status_dot_visible(s_view.claude_dot, snapshot->claude_unread);
}

static void refresh_weather_summary(const home_snapshot_t *snapshot)
{
    char weather_line[64];
    char city_name[24];
    bool has_weather = home_snapshot_has_weather(snapshot);
    lv_color_t weather_color =
        lv_color_hex(has_weather ? HOME_WEATHER_TEXT_COLOR : HOME_WEATHER_MUTED_COLOR);

    display_city_name(city_name, sizeof(city_name), snapshot->city_text);
    if (has_weather) {
        snprintf(weather_line, sizeof(weather_line), "%s  %d°C", city_name,
                 rounded_temperature_c(snapshot->temperature_c_tenths));
    } else {
        snprintf(weather_line, sizeof(weather_line), "%s  --", city_name);
    }

    if (snapshot->weather_stale) {
        snprintf(weather_line + strlen(weather_line), sizeof(weather_line) - strlen(weather_line),
                 "  cached");
    }

    lv_label_set_text_static(
        s_view.weather_icon,
        weather_icon_symbol(has_weather ? snapshot->weather_icon_id : WEATHER_ICON_UNKNOWN));
    lv_obj_set_style_text_color(s_view.weather_icon, weather_color, 0);
    lv_label_set_text(s_view.weather_label, weather_line);
    lv_obj_set_style_text_color(s_view.weather_label, weather_color, 0);
}

static void refresh_view(void)
{
    home_snapshot_t snapshot;
    char date_line[48];

    if (s_view.root == NULL) {
        return;
    }

    home_service_get_snapshot(&snapshot);
    snprintf(date_line, sizeof(date_line), "%s  %s", snapshot.date_text, snapshot.weekday_text);

    lv_label_set_text(s_view.time_label, snapshot.time_text);
    lv_label_set_text(s_view.date_label, date_line);
    refresh_weather_summary(&snapshot);
    refresh_status_bar(&snapshot);
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

    s_view.status_bar = lv_obj_create(root);
    lv_obj_remove_style_all(s_view.status_bar);
    lv_obj_set_size(s_view.status_bar, HOME_STATUS_BAR_WIDTH, HOME_STATUS_BAR_HEIGHT);
    lv_obj_align(s_view.status_bar, LV_ALIGN_TOP_LEFT, 0, HOME_STATUS_BAR_Y);
    lv_obj_set_flex_flow(s_view.status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_view.status_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_view.status_bar, HOME_STATUS_ICON_GAP, 0);

    s_view.wifi_item = lv_obj_create(s_view.status_bar);
    lv_obj_remove_style_all(s_view.wifi_item);
    lv_obj_set_size(s_view.wifi_item, HOME_STATUS_ITEM_BOX_SIZE, HOME_STATUS_ITEM_BOX_SIZE);

    s_view.wifi_icon = lv_label_create(s_view.wifi_item);
    lv_label_set_text_static(s_view.wifi_icon, APP_HOME_SYMBOL_WIFI);
    lv_obj_set_style_text_font(s_view.wifi_icon, &app_home_status_font, 0);
    lv_obj_align(s_view.wifi_icon, LV_ALIGN_CENTER, 0, 0);

    s_view.wifi_dot = lv_obj_create(s_view.wifi_item);
    lv_obj_remove_style_all(s_view.wifi_dot);
    lv_obj_set_size(s_view.wifi_dot, 4, 4);
    lv_obj_set_style_bg_color(s_view.wifi_dot, lv_color_hex(HOME_WIFI_CONNECTING_DOT_COLOR), 0);
    lv_obj_set_style_bg_opa(s_view.wifi_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_view.wifi_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(s_view.wifi_dot, LV_ALIGN_TOP_RIGHT, 0, 1);
    lv_obj_add_flag(s_view.wifi_dot, LV_OBJ_FLAG_HIDDEN);

    s_view.claude_item = lv_obj_create(s_view.status_bar);
    lv_obj_remove_style_all(s_view.claude_item);
    lv_obj_set_size(s_view.claude_item, HOME_STATUS_ITEM_BOX_SIZE, HOME_STATUS_ITEM_BOX_SIZE);

    s_view.claude_icon = lv_label_create(s_view.claude_item);
    lv_label_set_text_static(s_view.claude_icon, APP_HOME_SYMBOL_CLAUDE);
    lv_obj_set_style_text_font(s_view.claude_icon, &app_home_status_font, 0);
    lv_obj_align(s_view.claude_icon, LV_ALIGN_CENTER, 0, 0);

    s_view.claude_dot = lv_obj_create(s_view.claude_item);
    lv_obj_remove_style_all(s_view.claude_dot);
    lv_obj_set_size(s_view.claude_dot, 4, 4);
    lv_obj_set_style_bg_color(s_view.claude_dot, lv_color_hex(HOME_CLAUDE_UNREAD_DOT_COLOR), 0);
    lv_obj_set_style_bg_opa(s_view.claude_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_view.claude_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(s_view.claude_dot, LV_ALIGN_TOP_RIGHT, 0, 1);
    lv_obj_add_flag(s_view.claude_dot, LV_OBJ_FLAG_HIDDEN);

    s_view.time_label = lv_label_create(root);
    lv_obj_set_style_text_font(s_view.time_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_view.time_label, lv_color_hex(0xa6f0ff), 0);
    lv_obj_align(s_view.time_label, LV_ALIGN_TOP_LEFT, 0, HOME_TIME_Y);

    s_view.date_label = lv_label_create(root);
    lv_obj_set_style_text_font(s_view.date_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_view.date_label, lv_color_hex(0xb7c4cc), 0);
    lv_obj_align(s_view.date_label, LV_ALIGN_TOP_LEFT, 2, 94);

    s_view.weather_row = lv_obj_create(root);
    lv_obj_remove_style_all(s_view.weather_row);
    lv_obj_set_size(s_view.weather_row, BSP_LCD_H_RES - 32, HOME_WEATHER_ROW_HEIGHT);
    lv_obj_align(s_view.weather_row, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_flex_flow(s_view.weather_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_view.weather_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_view.weather_row, HOME_WEATHER_ICON_GAP, 0);

    s_view.weather_icon = lv_label_create(s_view.weather_row);
    lv_label_set_text_static(s_view.weather_icon, APP_HOME_SYMBOL_WEATHER_UNKNOWN);
    lv_obj_set_style_text_font(s_view.weather_icon, &app_home_status_font, 0);
    lv_obj_set_style_translate_y(s_view.weather_icon, HOME_WEATHER_ICON_Y_OFFSET, 0);

    s_view.weather_label = lv_label_create(s_view.weather_row);
    lv_obj_set_width(s_view.weather_label,
                     BSP_LCD_H_RES - 32 - HOME_STATUS_ITEM_BOX_SIZE - HOME_WEATHER_ICON_GAP);
    lv_label_set_long_mode(s_view.weather_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(s_view.weather_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_view.weather_label, lv_color_hex(HOME_WEATHER_TEXT_COLOR), 0);
    lv_obj_set_style_translate_y(s_view.weather_label, HOME_WEATHER_TEXT_Y_OFFSET, 0);

    refresh_view();
    return root;
}

static void app_home_resume(void)
{
    home_service_refresh_snapshot();
    refresh_view();
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
