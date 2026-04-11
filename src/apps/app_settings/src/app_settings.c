#include "app_settings.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "bsp_board_config.h"
#include "lvgl.h"
#include "service_settings.h"
#include "ui_fonts.h"

#define SETTINGS_BG_COLOR 0x0f1418
#define SETTINGS_RAIL_COLOR 0x141b20
#define SETTINGS_PANEL_COLOR 0x1b2328
#define SETTINGS_PANEL_ALT_COLOR 0x27333b
#define SETTINGS_ACCENT_COLOR 0x315965
#define SETTINGS_TEXT_COLOR 0xe4eef2
#define SETTINGS_MUTED_COLOR 0x8fa5ad
#define SETTINGS_DANGER_COLOR 0x7b3a3a
#define SETTINGS_OK_COLOR 0x2f6341
#define SETTINGS_WARNING_COLOR 0x8f6f2d
#define SETTINGS_RAIL_W 128
#define SETTINGS_STATUS_Y 6
#define SETTINGS_BODY_Y 22
#define SETTINGS_BODY_H (BSP_LCD_V_RES - SETTINGS_BODY_Y - 4)
#define SETTINGS_ROW_H 28
#define SETTINGS_SUMMARY_H 28
#define SETTINGS_KEYBOARD_H 76
#define SETTINGS_NAV_DEPTH_MAX 6
#define SETTINGS_ERROR_TEXT_MAX 96

typedef enum {
    SETTINGS_PAGE_INDEX = 0,
    SETTINGS_PAGE_WIFI_HOME,
    SETTINGS_PAGE_WIFI_PROFILES,
    SETTINGS_PAGE_WIFI_PROFILE_ACTIONS,
    SETTINGS_PAGE_WIFI_SCAN,
    SETTINGS_PAGE_WIFI_CONFIRM,
    SETTINGS_PAGE_WIFI_INPUT,
    SETTINGS_PAGE_TIME_HOME,
    SETTINGS_PAGE_WEATHER_HOME,
} settings_page_t;

typedef enum {
    SETTINGS_INPUT_NONE = 0,
    SETTINGS_INPUT_HIDDEN_SSID,
    SETTINGS_INPUT_HIDDEN_PASSWORD,
    SETTINGS_INPUT_PROFILE_PASSWORD,
} settings_input_mode_t;

typedef enum {
    SETTINGS_CONFIRM_NONE = 0,
    SETTINGS_CONFIRM_DELETE,
    SETTINGS_CONFIRM_USE_STORED_PASSWORD,
    SETTINGS_CONFIRM_OPEN_NETWORK,
    SETTINGS_CONFIRM_ERROR,
} settings_confirm_mode_t;

typedef struct {
    settings_page_t page;
    settings_input_mode_t input_mode;
    settings_confirm_mode_t confirm_mode;
} settings_nav_entry_t;

typedef struct {
    lv_obj_t *root;
    lv_obj_t *title_label;
    lv_obj_t *page_label;
    lv_obj_t *status_label;
    lv_obj_t *btn_back;
    lv_obj_t *btn_home;
    lv_obj_t *body;
    lv_obj_t *content_list;
    lv_obj_t *input_textarea;
    settings_snapshot_t snapshot;
    settings_nav_entry_t nav_stack[SETTINGS_NAV_DEPTH_MAX];
    size_t nav_depth;
    char selected_ssid[NET_MANAGER_SSID_MAX];
    char pending_ssid[NET_MANAGER_SSID_MAX];
    bool pending_hidden;
    char input_text[NET_MANAGER_PASSWORD_MAX];
    char error_message[SETTINGS_ERROR_TEXT_MAX];
} settings_runtime_t;

static const char *TAG = "app_settings";
static settings_runtime_t s_runtime;

static void copy_text(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0) {
        return;
    }

    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    snprintf(dst, dst_size, "%s", src);
}

static const char *net_state_text(net_state_t state)
{
    switch (state) {
    case NET_STATE_UP:
        return "connected";
    case NET_STATE_CONNECTING:
        return "connecting";
    default:
        return "down";
    }
}

static const net_profile_summary_t *find_selected_profile(const settings_runtime_t *runtime)
{
    size_t i;

    if (runtime == NULL) {
        return NULL;
    }

    for (i = 0; i < runtime->snapshot.profile_count; ++i) {
        if (strcmp(runtime->snapshot.profiles[i].ssid, runtime->selected_ssid) == 0) {
            return &runtime->snapshot.profiles[i];
        }
    }

    if (runtime->snapshot.profile_count > 0) {
        return &runtime->snapshot.profiles[0];
    }

    return NULL;
}

static const net_profile_summary_t *find_profile_by_ssid(const settings_runtime_t *runtime,
                                                         const char *ssid)
{
    size_t i;

    if (runtime == NULL || ssid == NULL || ssid[0] == '\0') {
        return NULL;
    }

    for (i = 0; i < runtime->snapshot.profile_count; ++i) {
        if (strcmp(runtime->snapshot.profiles[i].ssid, ssid) == 0) {
            return &runtime->snapshot.profiles[i];
        }
    }

    return NULL;
}

static void sync_selected_profile(settings_runtime_t *runtime)
{
    size_t i;

    if (runtime == NULL) {
        return;
    }

    if (runtime->snapshot.profile_count == 0) {
        runtime->selected_ssid[0] = '\0';
        return;
    }

    for (i = 0; i < runtime->snapshot.profile_count; ++i) {
        if (strcmp(runtime->snapshot.profiles[i].ssid, runtime->selected_ssid) == 0) {
            return;
        }
    }

    for (i = 0; i < runtime->snapshot.profile_count; ++i) {
        if (runtime->snapshot.profiles[i].active) {
            copy_text(runtime->selected_ssid, sizeof(runtime->selected_ssid),
                      runtime->snapshot.profiles[i].ssid);
            return;
        }
    }

    copy_text(runtime->selected_ssid, sizeof(runtime->selected_ssid),
              runtime->snapshot.profiles[0].ssid);
}

static int find_list_item_index(lv_obj_t *list, lv_obj_t *item)
{
    int32_t i = 0;

    if (list == NULL || item == NULL) {
        return -1;
    }

    for (;; ++i) {
        lv_obj_t *child = lv_obj_get_child(list, i);
        if (child == NULL) {
            break;
        }
        if (child == item) {
            return (int)i;
        }
    }

    return -1;
}

static const char *request_error_text(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_INVALID_STATE:
        return "Settings is busy";
    case ESP_ERR_TIMEOUT:
        return "Settings queue is full";
    case ESP_ERR_INVALID_ARG:
        return "Wi-Fi input is invalid";
    default:
        return "Settings request failed";
    }
}

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (obj == NULL) {
        return;
    }

    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_button_enabled(lv_obj_t *btn, bool enabled)
{
    if (btn == NULL) {
        return;
    }

    if (enabled) {
        lv_obj_clear_state(btn, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(btn, LV_STATE_DISABLED);
    }
}

static settings_page_t settings_current_page(const settings_runtime_t *runtime)
{
    if (runtime == NULL || runtime->nav_depth == 0) {
        return SETTINGS_PAGE_INDEX;
    }

    return runtime->nav_stack[runtime->nav_depth - 1].page;
}

static settings_input_mode_t settings_current_input_mode(const settings_runtime_t *runtime)
{
    if (runtime == NULL || runtime->nav_depth == 0 ||
        runtime->nav_stack[runtime->nav_depth - 1].page != SETTINGS_PAGE_WIFI_INPUT) {
        return SETTINGS_INPUT_NONE;
    }

    return runtime->nav_stack[runtime->nav_depth - 1].input_mode;
}

static settings_confirm_mode_t settings_current_confirm_mode(const settings_runtime_t *runtime)
{
    if (runtime == NULL || runtime->nav_depth == 0 ||
        runtime->nav_stack[runtime->nav_depth - 1].page != SETTINGS_PAGE_WIFI_CONFIRM) {
        return SETTINGS_CONFIRM_NONE;
    }

    return runtime->nav_stack[runtime->nav_depth - 1].confirm_mode;
}

static void settings_nav_set_entry(settings_runtime_t *runtime, size_t index, settings_page_t page,
                                   settings_input_mode_t input_mode,
                                   settings_confirm_mode_t confirm_mode)
{
    if (runtime == NULL || index >= SETTINGS_NAV_DEPTH_MAX) {
        return;
    }

    runtime->nav_stack[index] = (settings_nav_entry_t){
        .page = page,
        .input_mode = input_mode,
        .confirm_mode = confirm_mode,
    };
}

static void settings_nav_reset(settings_runtime_t *runtime)
{
    if (runtime == NULL) {
        return;
    }

    runtime->nav_depth = 1;
    settings_nav_set_entry(runtime, 0, SETTINGS_PAGE_INDEX, SETTINGS_INPUT_NONE,
                           SETTINGS_CONFIRM_NONE);
}

static void settings_nav_push(settings_runtime_t *runtime, settings_page_t page,
                              settings_input_mode_t input_mode,
                              settings_confirm_mode_t confirm_mode)
{
    if (runtime == NULL) {
        return;
    }

    if (runtime->nav_depth >= SETTINGS_NAV_DEPTH_MAX) {
        ESP_LOGW(TAG, "settings nav stack full; ignoring push to page %d", (int)page);
        return;
    }

    settings_nav_set_entry(runtime, runtime->nav_depth, page, input_mode, confirm_mode);
    runtime->nav_depth += 1;
}

static void settings_nav_pop(settings_runtime_t *runtime)
{
    if (runtime == NULL || runtime->nav_depth <= 1) {
        return;
    }

    runtime->nav_depth -= 1;
}

static void settings_nav_open_wifi_home(settings_runtime_t *runtime)
{
    if (runtime == NULL) {
        return;
    }

    runtime->nav_depth = 2;
    settings_nav_set_entry(runtime, 0, SETTINGS_PAGE_INDEX, SETTINGS_INPUT_NONE,
                           SETTINGS_CONFIRM_NONE);
    settings_nav_set_entry(runtime, 1, SETTINGS_PAGE_WIFI_HOME, SETTINGS_INPUT_NONE,
                           SETTINGS_CONFIRM_NONE);
}

static void settings_clear_transient_state(settings_runtime_t *runtime)
{
    if (runtime == NULL) {
        return;
    }

    runtime->pending_ssid[0] = '\0';
    runtime->pending_hidden = false;
    runtime->input_text[0] = '\0';
    runtime->error_message[0] = '\0';
    runtime->content_list = NULL;
    runtime->input_textarea = NULL;
}

static void settings_reset_to_index(settings_runtime_t *runtime)
{
    if (runtime == NULL) {
        return;
    }

    settings_clear_transient_state(runtime);
    settings_nav_reset(runtime);
}

static void format_status_line(const settings_runtime_t *runtime, char *out, size_t out_size)
{
    const char *ssid;
    const char *status_text;

    if (out == NULL || out_size == 0) {
        return;
    }

    if (runtime == NULL) {
        out[0] = '\0';
        return;
    }

    ssid = runtime->snapshot.net.ssid[0] != '\0' ? runtime->snapshot.net.ssid : "<none>";
    status_text =
        runtime->snapshot.status_text[0] != '\0' ? runtime->snapshot.status_text : "Ready";
    snprintf(out, out_size, "%s · %s · %s", net_state_text(runtime->snapshot.net.state), ssid,
             status_text);
}

static void settings_set_shell(settings_runtime_t *runtime, const char *title, const char *footer,
                               bool show_back)
{
    char status_line[128];

    if (runtime == NULL) {
        return;
    }

    (void)footer;
    lv_label_set_text(runtime->page_label, title != NULL ? title : "Settings");
    format_status_line(runtime, status_line, sizeof(status_line));
    lv_label_set_text(runtime->status_label, status_line);
    set_hidden(runtime->btn_back, !show_back);
}

static lv_obj_t *create_action_button(lv_obj_t *parent, const char *text, uint32_t color,
                                      lv_event_cb_t cb, settings_runtime_t *runtime)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_t *label = lv_label_create(btn);

    lv_obj_set_width(btn, 96);
    lv_obj_set_height(btn, 26);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 4, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, ui_font_text_11(), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
    lv_obj_center(label);
    if (cb != NULL) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, runtime);
    }
    return btn;
}

static lv_obj_t *create_page_panel(lv_obj_t *parent)
{
    lv_obj_t *panel = lv_obj_create(parent);

    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(panel, 8, 0);
    return panel;
}

static lv_obj_t *create_rows_container(lv_obj_t *parent, lv_coord_t y, lv_coord_t height)
{
    lv_obj_t *container = lv_obj_create(parent);

    lv_obj_remove_style_all(container);
    lv_obj_set_width(container, lv_pct(100));
    lv_obj_set_height(container, height);
    lv_obj_align(container, LV_ALIGN_TOP_LEFT, 0, y);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(container, 6, 0);
    return container;
}

static lv_obj_t *create_section_button(lv_obj_t *parent, const char *title, const char *detail,
                                       uint32_t color, lv_event_cb_t cb,
                                       settings_runtime_t *runtime)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_t *title_label = lv_label_create(btn);
    lv_obj_t *detail_label = lv_label_create(btn);

    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_set_height(btn, SETTINGS_ROW_H);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 4, 0);
    lv_obj_set_style_border_width(btn, 0, 0);

    lv_label_set_text(title_label, title);
    lv_obj_set_width(title_label, 260);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(title_label, ui_font_text_11(), 0);
    lv_obj_set_style_text_color(title_label, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
    lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 8, 0);

    lv_label_set_text(detail_label, detail != NULL ? detail : "");
    lv_obj_set_width(detail_label, 176);
    lv_label_set_long_mode(detail_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(detail_label, ui_font_text_11(), 0);
    lv_obj_set_style_text_color(detail_label, lv_color_hex(SETTINGS_MUTED_COLOR), 0);
    lv_obj_align(detail_label, LV_ALIGN_RIGHT_MID, -8, 0);

    if (cb != NULL) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, runtime);
    }
    return btn;
}

static void create_info_card(lv_obj_t *parent, const char *title, const char *body, uint32_t color)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_t *title_label = lv_label_create(card);
    lv_obj_t *body_label = lv_label_create(card);

    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, SETTINGS_SUMMARY_H);
    lv_obj_set_style_bg_color(card, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 4, 0);
    lv_obj_set_style_pad_hor(card, 8, 0);
    lv_obj_set_style_pad_ver(card, 5, 0);

    lv_label_set_text(title_label, title != NULL ? title : "");
    lv_obj_set_width(title_label, 240);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(title_label, ui_font_text_11(), 0);
    lv_obj_set_style_text_color(title_label, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
    lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 8, 0);

    lv_label_set_text(body_label, body != NULL ? body : "");
    lv_obj_set_width(body_label, 176);
    lv_label_set_long_mode(body_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(body_label, ui_font_text_11(), 0);
    lv_obj_set_style_text_color(body_label, lv_color_hex(SETTINGS_MUTED_COLOR), 0);
    lv_obj_align(body_label, LV_ALIGN_RIGHT_MID, -8, 0);
}

static void create_empty_state(lv_obj_t *parent, const char *title, const char *body)
{
    lv_obj_t *title_label = lv_label_create(parent);
    lv_obj_t *body_label = lv_label_create(parent);

    lv_label_set_text(title_label, title != NULL ? title : "");
    lv_obj_set_style_text_font(title_label, ui_font_text_11(), 0);
    lv_obj_set_style_text_color(title_label, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
    lv_obj_align(title_label, LV_ALIGN_CENTER, 0, -10);

    lv_label_set_text(body_label, body != NULL ? body : "");
    lv_obj_set_width(body_label, lv_pct(100));
    lv_label_set_long_mode(body_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(body_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(body_label, ui_font_text_11(), 0);
    lv_obj_set_style_text_color(body_label, lv_color_hex(SETTINGS_MUTED_COLOR), 0);
    lv_obj_align(body_label, LV_ALIGN_CENTER, 0, 12);
}

static void format_profile_detail(const net_profile_summary_t *profile, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }

    if (profile == NULL) {
        out[0] = '\0';
        return;
    }

    snprintf(out, out_size, "%s · %s%s", profile->hidden ? "hidden" : "visible",
             profile->has_password ? "stored password" : "no password",
             profile->active ? " · active" : "");
}

static void format_scan_detail(const settings_runtime_t *runtime, const net_scan_ap_t *result,
                               char *out, size_t out_size)
{
    const net_profile_summary_t *profile;
    const char *auth_text;
    const char *stored_text = "";

    if (out == NULL || out_size == 0) {
        return;
    }

    if (runtime == NULL || result == NULL) {
        out[0] = '\0';
        return;
    }

    profile = find_profile_by_ssid(runtime, result->ssid);
    auth_text = net_manager_scan_ap_auth_required(result) ? "secured" : "open";
    if (profile != NULL && profile->has_password) {
        stored_text = " · stored password";
    }

    snprintf(out, out_size, "%d dBm · %s%s", result->rssi, auth_text, stored_text);
}

static void settings_render(settings_runtime_t *runtime);

static void settings_show_error(settings_runtime_t *runtime, const char *message, esp_err_t err)
{
    if (runtime == NULL || message == NULL) {
        return;
    }

    runtime->snapshot.last_op_failed = true;
    copy_text(runtime->snapshot.status_text, sizeof(runtime->snapshot.status_text), message);
    copy_text(runtime->error_message, sizeof(runtime->error_message), message);

    if (settings_current_page(runtime) == SETTINGS_PAGE_WIFI_CONFIRM &&
        settings_current_confirm_mode(runtime) == SETTINGS_CONFIRM_ERROR) {
        settings_render(runtime);
    } else {
        settings_nav_push(runtime, SETTINGS_PAGE_WIFI_CONFIRM, SETTINGS_INPUT_NONE,
                          SETTINGS_CONFIRM_ERROR);
        settings_render(runtime);
    }

    ESP_LOGW(TAG, "%s: %s", message, esp_err_to_name(err));
}

static void settings_service_refresh(settings_runtime_t *runtime)
{
    if (runtime == NULL) {
        return;
    }

    settings_service_get_snapshot(&runtime->snapshot);
    sync_selected_profile(runtime);
    if (settings_current_page(runtime) == SETTINGS_PAGE_WIFI_PROFILE_ACTIONS &&
        find_selected_profile(runtime) == NULL) {
        settings_nav_open_wifi_home(runtime);
    }
    settings_render(runtime);
}

static void back_btn_cb(lv_event_t *e)
{
    settings_runtime_t *runtime = lv_event_get_user_data(e);

    if (runtime == NULL) {
        return;
    }

    runtime->input_text[0] = '\0';
    runtime->error_message[0] = '\0';
    settings_nav_pop(runtime);
    settings_render(runtime);
}

static void home_btn_cb(lv_event_t *e)
{
    settings_runtime_t *runtime = lv_event_get_user_data(e);
    esp_err_t err;

    if (runtime == NULL) {
        return;
    }

    err = app_manager_post_switch_to(APP_ID_HOME);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to switch back to home: %s", esp_err_to_name(err));
    }
}

static void index_wifi_cb(lv_event_t *e)
{
    settings_runtime_t *runtime = lv_event_get_user_data(e);

    if (runtime == NULL) {
        return;
    }

    settings_nav_open_wifi_home(runtime);
    settings_render(runtime);
}

static void index_time_cb(lv_event_t *e)
{
    settings_runtime_t *runtime = lv_event_get_user_data(e);

    if (runtime == NULL) {
        return;
    }

    settings_nav_push(runtime, SETTINGS_PAGE_TIME_HOME, SETTINGS_INPUT_NONE, SETTINGS_CONFIRM_NONE);
    settings_render(runtime);
}

static void index_weather_cb(lv_event_t *e)
{
    settings_runtime_t *runtime = lv_event_get_user_data(e);

    if (runtime == NULL) {
        return;
    }

    settings_nav_push(runtime, SETTINGS_PAGE_WEATHER_HOME, SETTINGS_INPUT_NONE,
                      SETTINGS_CONFIRM_NONE);
    settings_render(runtime);
}

static void wifi_profiles_entry_cb(lv_event_t *e)
{
    settings_runtime_t *runtime = lv_event_get_user_data(e);

    if (runtime == NULL) {
        return;
    }

    settings_nav_push(runtime, SETTINGS_PAGE_WIFI_PROFILES, SETTINGS_INPUT_NONE,
                      SETTINGS_CONFIRM_NONE);
    settings_render(runtime);
}

static void wifi_scan_again_cb(lv_event_t *e)
{
    settings_runtime_t *runtime = lv_event_get_user_data(e);
    esp_err_t err;

    if (runtime == NULL || runtime->snapshot.busy) {
        return;
    }

    err = settings_service_request_scan();
    if (err != ESP_OK) {
        settings_show_error(runtime, request_error_text(err), err);
        return;
    }

    settings_service_refresh(runtime);
}

static void wifi_scan_entry_cb(lv_event_t *e)
{
    settings_runtime_t *runtime = lv_event_get_user_data(e);
    esp_err_t err;

    if (runtime == NULL || runtime->snapshot.busy) {
        return;
    }

    if (settings_current_page(runtime) != SETTINGS_PAGE_WIFI_SCAN) {
        settings_nav_push(runtime, SETTINGS_PAGE_WIFI_SCAN, SETTINGS_INPUT_NONE,
                          SETTINGS_CONFIRM_NONE);
        settings_render(runtime);
    }

    err = settings_service_request_scan();
    if (err != ESP_OK) {
        settings_show_error(runtime, request_error_text(err), err);
        return;
    }

    settings_service_refresh(runtime);
}

static void wifi_hidden_entry_cb(lv_event_t *e)
{
    settings_runtime_t *runtime = lv_event_get_user_data(e);

    if (runtime == NULL || runtime->snapshot.busy) {
        return;
    }

    runtime->pending_hidden = true;
    runtime->pending_ssid[0] = '\0';
    runtime->input_text[0] = '\0';
    settings_nav_push(runtime, SETTINGS_PAGE_WIFI_INPUT, SETTINGS_INPUT_HIDDEN_SSID,
                      SETTINGS_CONFIRM_NONE);
    settings_render(runtime);
}

static void profile_row_cb(lv_event_t *e)
{
    settings_runtime_t *runtime = lv_event_get_user_data(e);
    lv_obj_t *target = lv_event_get_target_obj(e);
    int index;

    if (runtime == NULL || target == NULL || runtime->content_list == NULL) {
        return;
    }

    index = find_list_item_index(runtime->content_list, target);
    if (index < 0 || (size_t)index >= runtime->snapshot.profile_count) {
        return;
    }

    copy_text(runtime->selected_ssid, sizeof(runtime->selected_ssid),
              runtime->snapshot.profiles[index].ssid);
    settings_nav_push(runtime, SETTINGS_PAGE_WIFI_PROFILE_ACTIONS, SETTINGS_INPUT_NONE,
                      SETTINGS_CONFIRM_NONE);
    settings_render(runtime);
}

static void profile_update_password_cb(lv_event_t *e)
{
    settings_runtime_t *runtime = lv_event_get_user_data(e);
    const net_profile_summary_t *profile;

    if (runtime == NULL || runtime->snapshot.busy) {
        return;
    }

    profile = find_selected_profile(runtime);
    if (profile == NULL) {
        return;
    }

    copy_text(runtime->pending_ssid, sizeof(runtime->pending_ssid), profile->ssid);
    runtime->pending_hidden = profile->hidden;
    runtime->input_text[0] = '\0';
    settings_nav_push(runtime, SETTINGS_PAGE_WIFI_INPUT, SETTINGS_INPUT_PROFILE_PASSWORD,
                      SETTINGS_CONFIRM_NONE);
    settings_render(runtime);
}

static void profile_delete_cb(lv_event_t *e)
{
    settings_runtime_t *runtime = lv_event_get_user_data(e);
    const net_profile_summary_t *profile;

    if (runtime == NULL || runtime->snapshot.busy) {
        return;
    }

    profile = find_selected_profile(runtime);
    if (profile == NULL) {
        return;
    }

    copy_text(runtime->pending_ssid, sizeof(runtime->pending_ssid), profile->ssid);
    settings_nav_push(runtime, SETTINGS_PAGE_WIFI_CONFIRM, SETTINGS_INPUT_NONE,
                      SETTINGS_CONFIRM_DELETE);
    settings_render(runtime);
}

static void scan_row_cb(lv_event_t *e)
{
    settings_runtime_t *runtime = lv_event_get_user_data(e);
    lv_obj_t *target = lv_event_get_target_obj(e);
    const net_scan_ap_t *result;
    const net_profile_summary_t *profile;
    int index;

    if (runtime == NULL || target == NULL || runtime->content_list == NULL ||
        runtime->snapshot.busy) {
        return;
    }

    index = find_list_item_index(runtime->content_list, target);
    if (index < 0 || (size_t)index >= runtime->snapshot.scan_count) {
        return;
    }

    result = &runtime->snapshot.scan_results[index];
    profile = find_profile_by_ssid(runtime, result->ssid);
    copy_text(runtime->pending_ssid, sizeof(runtime->pending_ssid), result->ssid);
    runtime->pending_hidden = false;
    runtime->input_text[0] = '\0';

    if (!net_manager_scan_ap_auth_required(result)) {
        settings_nav_push(runtime, SETTINGS_PAGE_WIFI_CONFIRM, SETTINGS_INPUT_NONE,
                          SETTINGS_CONFIRM_OPEN_NETWORK);
        settings_render(runtime);
        return;
    }

    if (profile != NULL && profile->has_password) {
        settings_nav_push(runtime, SETTINGS_PAGE_WIFI_CONFIRM, SETTINGS_INPUT_NONE,
                          SETTINGS_CONFIRM_USE_STORED_PASSWORD);
        settings_render(runtime);
        return;
    }

    settings_nav_push(runtime, SETTINGS_PAGE_WIFI_INPUT, SETTINGS_INPUT_PROFILE_PASSWORD,
                      SETTINGS_CONFIRM_NONE);
    settings_render(runtime);
}

static void confirm_primary_cb(lv_event_t *e)
{
    settings_runtime_t *runtime = lv_event_get_user_data(e);
    settings_confirm_mode_t mode;
    esp_err_t err = ESP_OK;

    if (runtime == NULL) {
        return;
    }

    mode = settings_current_confirm_mode(runtime);
    switch (mode) {
    case SETTINGS_CONFIRM_DELETE:
        err = settings_service_request_remove(runtime->pending_ssid);
        if (err == ESP_OK) {
            settings_nav_open_wifi_home(runtime);
        }
        break;
    case SETTINGS_CONFIRM_USE_STORED_PASSWORD:
        err = settings_service_request_add_or_update(runtime->pending_ssid, NULL,
                                                     runtime->pending_hidden);
        if (err == ESP_OK) {
            copy_text(runtime->selected_ssid, sizeof(runtime->selected_ssid),
                      runtime->pending_ssid);
            settings_nav_open_wifi_home(runtime);
        }
        break;
    case SETTINGS_CONFIRM_OPEN_NETWORK:
        err = settings_service_request_add_or_update(runtime->pending_ssid, "", false);
        if (err == ESP_OK) {
            copy_text(runtime->selected_ssid, sizeof(runtime->selected_ssid),
                      runtime->pending_ssid);
            settings_nav_open_wifi_home(runtime);
        }
        break;
    case SETTINGS_CONFIRM_ERROR:
        runtime->error_message[0] = '\0';
        settings_nav_pop(runtime);
        settings_render(runtime);
        return;
    default:
        return;
    }

    if (err != ESP_OK) {
        settings_show_error(runtime, request_error_text(err), err);
        return;
    }

    settings_service_refresh(runtime);
}

static void confirm_secondary_cb(lv_event_t *e)
{
    settings_runtime_t *runtime = lv_event_get_user_data(e);

    if (runtime == NULL) {
        return;
    }

    if (settings_current_confirm_mode(runtime) == SETTINGS_CONFIRM_USE_STORED_PASSWORD) {
        settings_nav_pop(runtime);
        runtime->input_text[0] = '\0';
        settings_nav_push(runtime, SETTINGS_PAGE_WIFI_INPUT, SETTINGS_INPUT_PROFILE_PASSWORD,
                          SETTINGS_CONFIRM_NONE);
        settings_render(runtime);
        return;
    }

    settings_nav_pop(runtime);
    settings_render(runtime);
}

static void confirm_cancel_cb(lv_event_t *e)
{
    settings_runtime_t *runtime = lv_event_get_user_data(e);

    if (runtime == NULL) {
        return;
    }

    runtime->error_message[0] = '\0';
    settings_nav_pop(runtime);
    settings_render(runtime);
}

static void input_textarea_cb(lv_event_t *e)
{
    settings_runtime_t *runtime = lv_event_get_user_data(e);
    lv_obj_t *textarea = lv_event_get_target_obj(e);
    const char *text;

    if (runtime == NULL || textarea == NULL) {
        return;
    }

    text = lv_textarea_get_text(textarea);
    copy_text(runtime->input_text, sizeof(runtime->input_text), text);
}

static void input_keyboard_cb(lv_event_t *e)
{
    settings_runtime_t *runtime = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    settings_input_mode_t mode;
    const char *text;

    if (runtime == NULL) {
        return;
    }

    mode = settings_current_input_mode(runtime);
    text = runtime->input_textarea != NULL ? lv_textarea_get_text(runtime->input_textarea)
                                           : runtime->input_text;

    if (code == LV_EVENT_CANCEL) {
        runtime->input_text[0] = '\0';
        settings_nav_pop(runtime);
        settings_render(runtime);
        return;
    }
    if (code != LV_EVENT_READY) {
        return;
    }

    switch (mode) {
    case SETTINGS_INPUT_HIDDEN_SSID:
        if (text == NULL || text[0] == '\0') {
            settings_show_error(runtime, "SSID is required", ESP_ERR_INVALID_ARG);
            return;
        }
        if (strlen(text) >= NET_MANAGER_SSID_MAX) {
            settings_show_error(runtime, "SSID is too long", ESP_ERR_INVALID_ARG);
            return;
        }
        copy_text(runtime->pending_ssid, sizeof(runtime->pending_ssid), text);
        runtime->input_text[0] = '\0';
        settings_nav_push(runtime, SETTINGS_PAGE_WIFI_INPUT, SETTINGS_INPUT_HIDDEN_PASSWORD,
                          SETTINGS_CONFIRM_NONE);
        settings_render(runtime);
        return;
    case SETTINGS_INPUT_HIDDEN_PASSWORD:
    case SETTINGS_INPUT_PROFILE_PASSWORD: {
        esp_err_t err;

        if (text != NULL && strlen(text) >= NET_MANAGER_PASSWORD_MAX) {
            settings_show_error(runtime, "Password is too long", ESP_ERR_INVALID_ARG);
            return;
        }
        err = settings_service_request_add_or_update(
            runtime->pending_ssid, text != NULL ? text : "", runtime->pending_hidden);
        if (err != ESP_OK) {
            settings_show_error(runtime, request_error_text(err), err);
            return;
        }
        copy_text(runtime->selected_ssid, sizeof(runtime->selected_ssid), runtime->pending_ssid);
        runtime->input_text[0] = '\0';
        settings_nav_open_wifi_home(runtime);
        settings_service_refresh(runtime);
        return;
    }
    default:
        return;
    }
}

static void render_index_page(settings_runtime_t *runtime)
{
    lv_obj_t *panel;
    lv_obj_t *rows;
    char wifi_detail[64];

    settings_set_shell(runtime, "Settings", NULL, false);
    panel = create_page_panel(runtime->body);

    snprintf(wifi_detail, sizeof(wifi_detail), "%s · %u saved",
             net_state_text(runtime->snapshot.net.state),
             (unsigned)runtime->snapshot.profile_count);
    rows = create_rows_container(panel, 0, SETTINGS_BODY_H);
    create_section_button(rows, "Wi-Fi", wifi_detail, SETTINGS_ACCENT_COLOR, index_wifi_cb,
                          runtime);
    create_section_button(rows, "Time", "Planned next", SETTINGS_PANEL_ALT_COLOR, index_time_cb,
                          runtime);
    create_section_button(rows, "Weather", "Planned next", SETTINGS_PANEL_ALT_COLOR,
                          index_weather_cb, runtime);
}

static void render_wifi_home_page(settings_runtime_t *runtime)
{
    lv_obj_t *panel;
    lv_obj_t *rows;
    char title[NET_MANAGER_SSID_MAX + 16];
    char body[80];
    lv_obj_t *btn_profiles;
    lv_obj_t *btn_scan;
    lv_obj_t *btn_hidden;

    settings_set_shell(runtime, "Wi-Fi", NULL, true);
    panel = create_page_panel(runtime->body);

    snprintf(title, sizeof(title), "%s",
             runtime->snapshot.net.ssid[0] != '\0' ? runtime->snapshot.net.ssid
                                                   : "No active network");
    snprintf(body, sizeof(body), "%s · %u stored profile%s",
             net_state_text(runtime->snapshot.net.state), (unsigned)runtime->snapshot.profile_count,
             runtime->snapshot.profile_count == 1 ? "" : "s");
    create_info_card(panel, title, body, SETTINGS_ACCENT_COLOR);

    rows = create_rows_container(panel, SETTINGS_SUMMARY_H + 6,
                                 SETTINGS_BODY_H - SETTINGS_SUMMARY_H - 8);
    btn_profiles = create_section_button(rows, "Stored Profiles", "Review saved networks",
                                         SETTINGS_PANEL_ALT_COLOR, wifi_profiles_entry_cb, runtime);
    btn_scan = create_section_button(rows, "Visible Networks", "Scan nearby access points",
                                     SETTINGS_PANEL_ALT_COLOR, wifi_scan_entry_cb, runtime);
    btn_hidden = create_section_button(rows, "Add Hidden Network", "Enter SSID manually",
                                       SETTINGS_PANEL_ALT_COLOR, wifi_hidden_entry_cb, runtime);

    set_button_enabled(btn_profiles, !runtime->snapshot.busy);
    set_button_enabled(btn_scan, !runtime->snapshot.busy);
    set_button_enabled(btn_hidden, !runtime->snapshot.busy);
}

static void render_wifi_profiles_page(settings_runtime_t *runtime)
{
    lv_obj_t *panel;
    size_t i;

    settings_set_shell(runtime, "Stored Profiles", "Open one saved network to change it.", true);
    panel = create_page_panel(runtime->body);

    if (runtime->snapshot.profile_count == 0) {
        create_empty_state(panel, "No Profiles",
                           "Use Visible Networks or Add Hidden Network first.");
        return;
    }

    runtime->content_list = lv_list_create(panel);
    lv_obj_set_size(runtime->content_list, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(runtime->content_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(runtime->content_list, 0, 0);
    lv_obj_set_style_pad_row(runtime->content_list, 6, 0);

    for (i = 0; i < runtime->snapshot.profile_count; ++i) {
        char detail[80];
        lv_obj_t *btn;

        format_profile_detail(&runtime->snapshot.profiles[i], detail, sizeof(detail));
        btn = create_section_button(
            runtime->content_list, runtime->snapshot.profiles[i].ssid, detail,
            strcmp(runtime->snapshot.profiles[i].ssid, runtime->selected_ssid) == 0
                ? SETTINGS_ACCENT_COLOR
                : SETTINGS_PANEL_ALT_COLOR,
            profile_row_cb, runtime);
        set_button_enabled(btn, !runtime->snapshot.busy);
    }
}

static void render_wifi_profile_actions_page(settings_runtime_t *runtime)
{
    lv_obj_t *panel;
    lv_obj_t *rows;
    const net_profile_summary_t *profile = find_selected_profile(runtime);
    char detail[80];
    lv_obj_t *btn_update;
    lv_obj_t *btn_delete;

    settings_set_shell(runtime, "Profile", NULL, true);
    panel = create_page_panel(runtime->body);

    if (profile == NULL) {
        create_empty_state(panel, "No Selection", "Pick a stored profile first.");
        return;
    }

    format_profile_detail(profile, detail, sizeof(detail));
    create_info_card(panel, profile->ssid, detail, SETTINGS_PANEL_ALT_COLOR);

    rows = create_rows_container(panel, SETTINGS_SUMMARY_H + 6,
                                 SETTINGS_BODY_H - SETTINGS_SUMMARY_H - 8);
    btn_update =
        create_section_button(rows, "Update Password", "Save a new password",
                              SETTINGS_PANEL_ALT_COLOR, profile_update_password_cb, runtime);
    btn_delete = create_section_button(rows, "Delete Profile", "Remove it from stored profiles",
                                       SETTINGS_DANGER_COLOR, profile_delete_cb, runtime);

    set_button_enabled(btn_update, !runtime->snapshot.busy);
    set_button_enabled(btn_delete, !runtime->snapshot.busy);
}

static void render_wifi_scan_page(settings_runtime_t *runtime)
{
    lv_obj_t *panel;
    lv_obj_t *scan_again;
    lv_obj_t *hint_label;
    size_t i;

    settings_set_shell(runtime, "Visible Networks", NULL, true);
    panel = create_page_panel(runtime->body);

    hint_label = lv_label_create(panel);
    lv_obj_set_width(hint_label, 260);
    lv_label_set_long_mode(hint_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(hint_label, ui_font_text_11(), 0);
    lv_obj_set_style_text_color(
        hint_label,
        lv_color_hex(runtime->snapshot.busy ? SETTINGS_WARNING_COLOR : SETTINGS_MUTED_COLOR), 0);
    scan_again = create_action_button(panel, "Scan Again", SETTINGS_ACCENT_COLOR,
                                      wifi_scan_again_cb, runtime);
    lv_obj_align(scan_again, LV_ALIGN_TOP_RIGHT, 0, 0);
    set_button_enabled(scan_again, !runtime->snapshot.busy);
    lv_obj_set_width(scan_again, 88);

    if (runtime->snapshot.busy && runtime->snapshot.scan_count > 0) {
        lv_label_set_text(hint_label, "Refreshing previous results...");
    } else if (runtime->snapshot.busy) {
        lv_label_set_text(hint_label, "Scanning nearby networks...");
    } else {
        lv_label_set_text(hint_label, "Select a network or refresh.");
    }
    lv_obj_align(hint_label, LV_ALIGN_TOP_LEFT, 0, 6);

    if (runtime->snapshot.scan_count == 0) {
        create_empty_state(panel, runtime->snapshot.busy ? "Scanning..." : "No Results",
                           runtime->snapshot.busy ? "Looking for nearby networks."
                                                  : "No visible networks were found yet.");
        return;
    }

    runtime->content_list = lv_list_create(panel);
    lv_obj_set_size(runtime->content_list, lv_pct(100), SETTINGS_BODY_H - 40);
    lv_obj_align(runtime->content_list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(runtime->content_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(runtime->content_list, 0, 0);
    lv_obj_set_style_pad_row(runtime->content_list, 6, 0);

    for (i = 0; i < runtime->snapshot.scan_count; ++i) {
        char detail[80];
        lv_obj_t *btn;

        format_scan_detail(runtime, &runtime->snapshot.scan_results[i], detail, sizeof(detail));
        btn = create_section_button(runtime->content_list, runtime->snapshot.scan_results[i].ssid,
                                    detail, SETTINGS_PANEL_ALT_COLOR, scan_row_cb, runtime);
        set_button_enabled(btn, !runtime->snapshot.busy);
    }
}

static void render_confirm_page(settings_runtime_t *runtime)
{
    lv_obj_t *panel;
    lv_obj_t *message_label;
    lv_obj_t *buttons;
    lv_obj_t *primary_btn;
    lv_obj_t *secondary_btn = NULL;
    lv_obj_t *cancel_btn = NULL;
    char message[SETTINGS_ERROR_TEXT_MAX];
    const char *title = "Confirm";
    const char *primary = "OK";
    const char *secondary = NULL;
    bool show_cancel = true;
    settings_confirm_mode_t mode = settings_current_confirm_mode(runtime);

    message[0] = '\0';
    switch (mode) {
    case SETTINGS_CONFIRM_DELETE:
        title = "Remove Profile";
        snprintf(message, sizeof(message), "Remove `%s` from stored profiles?",
                 runtime->pending_ssid);
        primary = "Delete";
        break;
    case SETTINGS_CONFIRM_USE_STORED_PASSWORD:
        title = "Use Stored Password";
        snprintf(message, sizeof(message), "Use the saved password for `%s`?",
                 runtime->pending_ssid);
        primary = "Use Stored";
        secondary = "Edit Password";
        break;
    case SETTINGS_CONFIRM_OPEN_NETWORK:
        title = "Open Network";
        snprintf(message, sizeof(message), "Save and connect to open network `%s`?",
                 runtime->pending_ssid);
        primary = "Save / Connect";
        break;
    case SETTINGS_CONFIRM_ERROR:
        title = "Notice";
        copy_text(message, sizeof(message),
                  runtime->error_message[0] != '\0' ? runtime->error_message
                                                    : "Settings request failed");
        primary = "OK";
        show_cancel = false;
        break;
    default:
        copy_text(message, sizeof(message), "Confirm the next step.");
        break;
    }

    settings_set_shell(runtime, title, NULL, true);
    panel = create_page_panel(runtime->body);

    message_label = lv_label_create(panel);
    lv_obj_set_width(message_label, lv_pct(100));
    lv_label_set_long_mode(message_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(message_label, message);
    lv_obj_set_style_text_font(message_label, ui_font_text_11(), 0);
    lv_obj_set_style_text_color(message_label, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
    lv_obj_align(message_label, LV_ALIGN_TOP_LEFT, 0, 4);

    buttons = lv_obj_create(panel);
    lv_obj_remove_style_all(buttons);
    lv_obj_set_width(buttons, lv_pct(100));
    lv_obj_set_height(buttons, 34);
    lv_obj_align(buttons, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(buttons, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(buttons, 8, 0);

    primary_btn = create_action_button(buttons, primary,
                                       mode == SETTINGS_CONFIRM_DELETE ? SETTINGS_DANGER_COLOR
                                                                       : SETTINGS_OK_COLOR,
                                       confirm_primary_cb, runtime);
    set_button_enabled(primary_btn, !runtime->snapshot.busy || mode == SETTINGS_CONFIRM_ERROR);

    if (secondary != NULL) {
        secondary_btn = create_action_button(buttons, secondary, SETTINGS_ACCENT_COLOR,
                                             confirm_secondary_cb, runtime);
        set_button_enabled(secondary_btn, !runtime->snapshot.busy);
    }
    if (show_cancel) {
        cancel_btn = create_action_button(buttons, "Back", SETTINGS_PANEL_ALT_COLOR,
                                          confirm_cancel_cb, runtime);
        set_button_enabled(cancel_btn, !runtime->snapshot.busy);
    }
}

static void render_input_page(settings_runtime_t *runtime)
{
    lv_obj_t *panel;
    lv_obj_t *prompt_label;
    lv_obj_t *keyboard;
    const char *title = "Input";
    const char *prompt = "";
    const char *initial_text = runtime->input_text;
    settings_input_mode_t mode = settings_current_input_mode(runtime);
    bool password = false;
    uint32_t max_length = NET_MANAGER_PASSWORD_MAX - 1;

    switch (mode) {
    case SETTINGS_INPUT_HIDDEN_SSID:
        title = "Hidden SSID";
        prompt = "Enter the exact network name.";
        if (initial_text[0] == '\0' && runtime->pending_ssid[0] != '\0') {
            initial_text = runtime->pending_ssid;
        }
        max_length = NET_MANAGER_SSID_MAX - 1;
        break;
    case SETTINGS_INPUT_HIDDEN_PASSWORD:
        title = "Hidden Password";
        prompt = runtime->pending_ssid[0] != '\0' ? runtime->pending_ssid : "Enter the password.";
        password = true;
        break;
    case SETTINGS_INPUT_PROFILE_PASSWORD:
        title = "Wi-Fi Password";
        prompt = runtime->pending_ssid[0] != '\0' ? runtime->pending_ssid : "Enter the password.";
        password = true;
        break;
    default:
        break;
    }

    settings_set_shell(runtime, title, NULL, true);
    panel = create_page_panel(runtime->body);

    prompt_label = lv_label_create(panel);
    lv_obj_set_width(prompt_label, lv_pct(100));
    lv_label_set_long_mode(prompt_label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(prompt_label, prompt);
    lv_obj_set_style_text_font(prompt_label, ui_font_text_11(), 0);
    lv_obj_set_style_text_color(prompt_label, lv_color_hex(SETTINGS_MUTED_COLOR), 0);
    lv_obj_align(prompt_label, LV_ALIGN_TOP_LEFT, 0, 0);

    runtime->input_textarea = lv_textarea_create(panel);
    lv_obj_set_size(runtime->input_textarea, lv_pct(100), 28);
    lv_obj_align(runtime->input_textarea, LV_ALIGN_TOP_MID, 0, 18);
    lv_textarea_set_one_line(runtime->input_textarea, true);
    lv_textarea_set_password_mode(runtime->input_textarea, password);
    lv_textarea_set_max_length(runtime->input_textarea, max_length);
    lv_textarea_set_text(runtime->input_textarea, initial_text != NULL ? initial_text : "");
    lv_obj_set_style_text_font(runtime->input_textarea, ui_font_text_11(), 0);
    lv_obj_add_event_cb(runtime->input_textarea, input_textarea_cb, LV_EVENT_VALUE_CHANGED,
                        runtime);

    keyboard = lv_keyboard_create(panel);
    lv_obj_set_size(keyboard, lv_pct(100), SETTINGS_KEYBOARD_H);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(keyboard, runtime->input_textarea);
    lv_obj_add_event_cb(keyboard, input_keyboard_cb, LV_EVENT_READY, runtime);
    lv_obj_add_event_cb(keyboard, input_keyboard_cb, LV_EVENT_CANCEL, runtime);
}

static void render_placeholder_page(settings_runtime_t *runtime, const char *title,
                                    const char *body)
{
    lv_obj_t *panel;

    settings_set_shell(runtime, title, NULL, true);
    panel = create_page_panel(runtime->body);
    create_empty_state(panel, title, body);
}

static void settings_render(settings_runtime_t *runtime)
{
    if (runtime == NULL || runtime->root == NULL) {
        return;
    }

    lv_obj_clean(runtime->body);
    runtime->content_list = NULL;
    runtime->input_textarea = NULL;

    switch (settings_current_page(runtime)) {
    case SETTINGS_PAGE_INDEX:
        render_index_page(runtime);
        break;
    case SETTINGS_PAGE_WIFI_HOME:
        render_wifi_home_page(runtime);
        break;
    case SETTINGS_PAGE_WIFI_PROFILES:
        render_wifi_profiles_page(runtime);
        break;
    case SETTINGS_PAGE_WIFI_PROFILE_ACTIONS:
        render_wifi_profile_actions_page(runtime);
        break;
    case SETTINGS_PAGE_WIFI_SCAN:
        render_wifi_scan_page(runtime);
        break;
    case SETTINGS_PAGE_WIFI_CONFIRM:
        render_confirm_page(runtime);
        break;
    case SETTINGS_PAGE_WIFI_INPUT:
        render_input_page(runtime);
        break;
    case SETTINGS_PAGE_TIME_HOME:
        render_placeholder_page(runtime, "Time",
                                "Time settings will move here once the Wi-Fi flow is stable.");
        break;
    case SETTINGS_PAGE_WEATHER_HOME:
        render_placeholder_page(runtime, "Weather",
                                "Weather settings will move here once the Wi-Fi flow is stable.");
        break;
    default:
        render_index_page(runtime);
        break;
    }
}

static lv_obj_t *app_settings_create_root(lv_obj_t *parent)
{
    memset(&s_runtime, 0, sizeof(s_runtime));

    settings_nav_reset(&s_runtime);

    s_runtime.root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_runtime.root);
    lv_obj_set_size(s_runtime.root, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(s_runtime.root, lv_color_hex(SETTINGS_BG_COLOR), 0);
    lv_obj_set_style_bg_opa(s_runtime.root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_runtime.root, 0, 0);

    {
        lv_obj_t *rail = lv_obj_create(s_runtime.root);
        lv_obj_t *nav_box;

        lv_obj_remove_style_all(rail);
        lv_obj_set_size(rail, SETTINGS_RAIL_W, BSP_LCD_V_RES);
        lv_obj_set_style_bg_color(rail, lv_color_hex(SETTINGS_RAIL_COLOR), 0);
        lv_obj_set_style_bg_opa(rail, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(rail, 0, 0);
        lv_obj_align(rail, LV_ALIGN_TOP_LEFT, 0, 0);

        s_runtime.title_label = lv_label_create(rail);
        lv_label_set_text(s_runtime.title_label, "SETTINGS");
        lv_obj_set_style_text_font(s_runtime.title_label, ui_font_text_11(), 0);
        lv_obj_set_style_text_color(s_runtime.title_label, lv_color_hex(SETTINGS_MUTED_COLOR), 0);
        lv_obj_align(s_runtime.title_label, LV_ALIGN_TOP_LEFT, 8, 8);

        s_runtime.page_label = lv_label_create(rail);
        lv_obj_set_width(s_runtime.page_label, SETTINGS_RAIL_W - 16);
        lv_label_set_long_mode(s_runtime.page_label, LV_LABEL_LONG_WRAP);
        lv_label_set_text(s_runtime.page_label, "Settings");
        lv_obj_set_style_text_font(s_runtime.page_label, ui_font_text_11(), 0);
        lv_obj_set_style_text_color(s_runtime.page_label, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
        lv_obj_align(s_runtime.page_label, LV_ALIGN_TOP_LEFT, 8, 24);

        nav_box = lv_obj_create(rail);
        lv_obj_remove_style_all(nav_box);
        lv_obj_set_size(nav_box, SETTINGS_RAIL_W - 16, 58);
        lv_obj_align(nav_box, LV_ALIGN_BOTTOM_MID, 0, -8);
        lv_obj_set_flex_flow(nav_box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(nav_box, 6, 0);

        s_runtime.btn_back = create_action_button(nav_box, "Back", SETTINGS_PANEL_ALT_COLOR,
                                                  back_btn_cb, &s_runtime);
        lv_obj_set_width(s_runtime.btn_back, lv_pct(100));
        s_runtime.btn_home = create_action_button(nav_box, "Home", SETTINGS_PANEL_ALT_COLOR,
                                                  home_btn_cb, &s_runtime);
        lv_obj_set_width(s_runtime.btn_home, lv_pct(100));
    }

    s_runtime.status_label = lv_label_create(s_runtime.root);
    lv_obj_set_width(s_runtime.status_label, BSP_LCD_H_RES - SETTINGS_RAIL_W - 16);
    lv_label_set_long_mode(s_runtime.status_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(s_runtime.status_label, ui_font_text_11(), 0);
    lv_obj_set_style_text_color(s_runtime.status_label, lv_color_hex(SETTINGS_MUTED_COLOR), 0);
    lv_obj_align(s_runtime.status_label, LV_ALIGN_TOP_LEFT, SETTINGS_RAIL_W + 8, SETTINGS_STATUS_Y);

    s_runtime.body = lv_obj_create(s_runtime.root);
    lv_obj_remove_style_all(s_runtime.body);
    lv_obj_set_size(s_runtime.body, BSP_LCD_H_RES - SETTINGS_RAIL_W - 8, SETTINGS_BODY_H);
    lv_obj_set_style_bg_color(s_runtime.body, lv_color_hex(SETTINGS_PANEL_COLOR), 0);
    lv_obj_set_style_bg_opa(s_runtime.body, LV_OPA_COVER, 0);
    lv_obj_align(s_runtime.body, LV_ALIGN_TOP_LEFT, SETTINGS_RAIL_W + 4, SETTINGS_BODY_Y);

    settings_service_refresh(&s_runtime);
    return s_runtime.root;
}

static esp_err_t app_settings_init(void) { return ESP_OK; }

static void app_settings_resume(void)
{
    settings_reset_to_index(&s_runtime);
    settings_service_refresh(&s_runtime);
}

static void app_settings_suspend(void) { settings_reset_to_index(&s_runtime); }

static void app_settings_handle_event(const app_event_t *event)
{
    if (event == NULL) {
        return;
    }

    switch (event->type) {
    case APP_EVENT_ENTER:
    case APP_EVENT_DATA_SETTINGS:
    case APP_EVENT_NET_CHANGED:
        settings_service_refresh(&s_runtime);
        break;
    default:
        break;
    }
}

const app_descriptor_t *app_settings_get_descriptor(void)
{
    static const app_descriptor_t descriptor = {
        .id = APP_ID_SETTINGS,
        .name = "Settings",
        .init = app_settings_init,
        .create_root = app_settings_create_root,
        .resume = app_settings_resume,
        .suspend = app_settings_suspend,
        .handle_event = app_settings_handle_event,
    };

    return &descriptor;
}
