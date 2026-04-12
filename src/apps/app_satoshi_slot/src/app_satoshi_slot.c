#include "app_satoshi_slot.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "bsp_board_config.h"
#include "lvgl.h"
#include "service_bitcoin.h"
#include "ui_theme.h"
#include "ui_fonts.h"

#define SLOT_BG ui_theme_color_hex(UI_THEME_COLOR_CANVAS_BG)
#define SLOT_PANEL ui_theme_color_hex(UI_THEME_COLOR_SURFACE_PRIMARY)
#define SLOT_PANEL_ALT ui_theme_color_hex(UI_THEME_COLOR_SURFACE_SECONDARY)
#define SLOT_TEXT ui_theme_color_hex(UI_THEME_COLOR_TEXT_PRIMARY)
#define SLOT_MUTED ui_theme_color_hex(UI_THEME_COLOR_TEXT_MUTED)
#define SLOT_ACTIVE ui_theme_color_hex(UI_THEME_COLOR_ACCENT_PRIMARY)
#define SLOT_DANGER ui_theme_color_hex(UI_THEME_COLOR_STATUS_ERROR)
#define SLOT_OK ui_theme_color_hex(UI_THEME_COLOR_STATUS_SUCCESS)
#define SLOT_PAD_X 14
#define SLOT_PAD_TOP 10
#define SLOT_PAD_BOTTOM 10
#define SLOT_GAP_X 12
#define SLOT_GAP_Y 8
#define SLOT_HEADER_H 26
#define SLOT_BUTTON_H 28
#define SLOT_BUTTON_GAP 8
#define SLOT_RIGHT_PANEL_W 244
#define SLOT_CONTENT_Y (SLOT_PAD_TOP + SLOT_HEADER_H + SLOT_GAP_Y)
#define SLOT_CONTENT_H                                                                             \
    (BSP_LCD_V_RES - SLOT_PAD_TOP - SLOT_PAD_BOTTOM - SLOT_HEADER_H - SLOT_BUTTON_H -              \
     (SLOT_GAP_Y * 2))
#define SLOT_LEFT_COL_W ((BSP_LCD_H_RES - (SLOT_PAD_X * 2)) - SLOT_RIGHT_PANEL_W - SLOT_GAP_X)
#define SLOT_RIGHT_PANEL_X (SLOT_PAD_X + SLOT_LEFT_COL_W + SLOT_GAP_X)
#define SLOT_BUTTON_Y (SLOT_CONTENT_Y + SLOT_CONTENT_H + SLOT_GAP_Y)
#define SLOT_BTN_START_W 92
#define SLOT_BTN_PAUSE_W 92
#define SLOT_BTN_SELFTEST_W 104
#define SLOT_BTN_RESET_W 92
#define SLOT_BTN_CONFIRM_W 110
#define SLOT_BTN_X0 SLOT_PAD_X
#define SLOT_BTN_X1 (SLOT_BTN_X0 + SLOT_BTN_START_W + SLOT_BUTTON_GAP)
#define SLOT_BTN_X2 (SLOT_BTN_X1 + SLOT_BTN_PAUSE_W + SLOT_BUTTON_GAP)
#define SLOT_BTN_X3 (SLOT_BTN_X2 + SLOT_BTN_SELFTEST_W + SLOT_BUTTON_GAP)
#define SLOT_BTN_X4 (SLOT_BTN_X3 + SLOT_BTN_RESET_W + SLOT_BUTTON_GAP)

typedef struct {
    lv_obj_t *root;
    lv_obj_t *state_label;
    lv_obj_t *detail_label;
    lv_obj_t *metrics_label;
    lv_obj_t *prefix_label;
    lv_obj_t *panel_label;
    lv_obj_t *btn_start;
    lv_obj_t *btn_pause;
    lv_obj_t *btn_selftest;
    lv_obj_t *btn_reset;
    lv_obj_t *btn_confirm;
} slot_view_t;

static slot_view_t s_view;

static const char *slot_status_text(const slot_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return "READY";
    }

    switch (snapshot->state) {
    case SLOT_STATE_RUNNING:
        return snapshot->mode == SLOT_MODE_SELFTEST ? "SELF-TEST" : "RUNNING";
    case SLOT_STATE_HIT:
        return snapshot->hit_persisted ? "HIT SAVED" : "HIT UNSAVED";
    case SLOT_STATE_STORAGE_UNAVAILABLE:
        return "SETUP NEEDED";
    case SLOT_STATE_ERROR:
        return "ERROR";
    case SLOT_STATE_PAUSED:
        return "PAUSED";
    case SLOT_STATE_IDLE:
    default:
        return "READY";
    }
}

static void slot_format_detail(const slot_snapshot_t *snapshot, const char *label_name, char *out,
                               size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }

    if (snapshot == NULL) {
        snprintf(out, out_size, "%s", "Waiting for slot data.");
        return;
    }

    switch (snapshot->state) {
    case SLOT_STATE_RUNNING:
        snprintf(out, out_size, "%s scan is running on the Genesis target set.",
                 snapshot->mode == SLOT_MODE_SELFTEST ? "Self-test" : "Normal");
        break;
    case SLOT_STATE_STORAGE_UNAVAILABLE:
        snprintf(out, out_size, "%s",
                 "Normal scan needs encrypted storage. Self-test still works.");
        break;
    case SLOT_STATE_HIT:
        if (snapshot->is_self_test_hit) {
            snprintf(out, out_size, "Self-test matched %s. Confirm to reset.", label_name);
        } else {
            snprintf(out, out_size, "Matched %s. Key %s. Confirm to acknowledge.", label_name,
                     snapshot->hit_persisted ? "persisted" : "NOT persisted");
        }
        break;
    case SLOT_STATE_ERROR:
        snprintf(out, out_size, "%s", "Derive or persist pipeline failed.");
        break;
    case SLOT_STATE_PAUSED:
        snprintf(out, out_size, "%s",
                 snapshot->matched_label_id != 0 ? "Paused after a recorded hit."
                                                 : "Paused by user or power policy.");
        break;
    case SLOT_STATE_IDLE:
    default:
        snprintf(out, out_size, "%s", "Ready. Start normal scan or run self-test.");
        break;
    }
}

static void slot_format_panel_text(const slot_snapshot_t *snapshot, const char *label_name,
                                   char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }

    if (snapshot == NULL) {
        snprintf(out, out_size, "%s", "No slot data yet.");
        return;
    }

    switch (snapshot->state) {
    case SLOT_STATE_RUNNING:
        snprintf(out, out_size, "%s mode\nGenesis target set",
                 snapshot->mode == SLOT_MODE_SELFTEST ? "Self-test" : "Normal");
        break;
    case SLOT_STATE_PAUSED:
        if (snapshot->matched_label_id != 0) {
            snprintf(out, out_size, "Last hit: %s\nsaved: %s", label_name,
                     snapshot->hit_persisted ? "yes" : "no");
        } else {
            snprintf(out, out_size, "%s", "Scan paused.\nCounters stay intact.");
        }
        break;
    case SLOT_STATE_HIT:
        if (snapshot->is_self_test_hit) {
            snprintf(out, out_size, "SELF-TEST HIT\n%s\nsaved: %s", label_name,
                     snapshot->hit_persisted ? "yes" : "no");
        } else {
            snprintf(out, out_size, "REAL HIT\n%s\nsaved: %s\n\nesp32dash slot export", label_name,
                     snapshot->hit_persisted ? "yes" : "no");
        }
        break;
    case SLOT_STATE_STORAGE_UNAVAILABLE:
        snprintf(out, out_size, "%s", "Normal scan locked.\nEncrypted storage is not ready.");
        break;
    case SLOT_STATE_ERROR:
        snprintf(out, out_size, "%s", "Slot pipeline error.\nReset after recovery.");
        break;
    case SLOT_STATE_IDLE:
    default:
        snprintf(out, out_size, "%s",
                 "No hit recorded yet.\nSelf-test exercises the full pipeline.");
        break;
    }
}

static lv_color_t slot_state_color(const slot_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return lv_color_hex(SLOT_MUTED);
    }

    switch (snapshot->state) {
    case SLOT_STATE_RUNNING:
        return lv_color_hex(SLOT_ACTIVE);
    case SLOT_STATE_PAUSED:
        return lv_color_hex(SLOT_MUTED);
    case SLOT_STATE_HIT:
        return snapshot->hit_persisted ? lv_color_hex(SLOT_OK) : lv_color_hex(SLOT_DANGER);
    case SLOT_STATE_STORAGE_UNAVAILABLE:
        return lv_color_hex(SLOT_DANGER);
    case SLOT_STATE_ERROR:
        return lv_color_hex(SLOT_DANGER);
    case SLOT_STATE_IDLE:
    default:
        return lv_color_hex(SLOT_MUTED);
    }
}

static lv_obj_t *create_action_button(lv_obj_t *parent, const char *text)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_t *label = lv_label_create(btn);

    lv_obj_set_size(btn, SLOT_BTN_START_W, SLOT_BUTTON_H);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(SLOT_PANEL_ALT), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, ui_theme_color(UI_THEME_COLOR_SURFACE_ACTIVE), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, lv_color_hex(SLOT_PANEL), LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_DISABLED);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, ui_font_text_11(), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(SLOT_TEXT), 0);
    lv_obj_center(label);
    return btn;
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

static void slot_apply_snapshot(void)
{
    slot_snapshot_t snapshot;
    char detail[96];
    char metrics[128];
    char panel[128];
    char prefix[32];
    const char *label_name;
    bool show_confirm;
    bool start_enabled;

    if (s_view.root == NULL) {
        return;
    }

    bitcoin_service_get_slot_snapshot(&snapshot);
    lv_label_set_text(s_view.state_label, slot_status_text(&snapshot));
    lv_obj_set_style_text_color(s_view.state_label, slot_state_color(&snapshot), 0);

    label_name = bitcoin_service_label_for_id(snapshot.matched_label_id);
    slot_format_detail(&snapshot, label_name, detail, sizeof(detail));
    slot_format_panel_text(&snapshot, label_name, panel, sizeof(panel));

    snprintf(metrics, sizeof(metrics), "attempts %llu  keys/s %" PRIu32 "  batch %" PRIu32,
             (unsigned long long)snapshot.attempts, snapshot.keys_per_sec, snapshot.batch_size);
    snprintf(prefix, sizeof(prefix), "last fp %02x%02x%02x%02x", snapshot.last_hash160_prefix[0],
             snapshot.last_hash160_prefix[1], snapshot.last_hash160_prefix[2],
             snapshot.last_hash160_prefix[3]);

    lv_label_set_text(s_view.detail_label, detail);
    lv_label_set_text(s_view.metrics_label, metrics);
    lv_label_set_text(s_view.prefix_label, prefix);
    lv_label_set_text(s_view.panel_label, panel);

    start_enabled = snapshot.secure_storage_ready;
    set_button_enabled(s_view.btn_start, start_enabled);
    lv_obj_set_style_bg_color(s_view.btn_start,
                              lv_color_hex(start_enabled ? SLOT_PANEL_ALT : SLOT_PANEL), 0);
    lv_obj_set_style_bg_color(s_view.btn_selftest, lv_color_hex(SLOT_PANEL_ALT), 0);
    lv_obj_set_style_bg_color(s_view.btn_pause, lv_color_hex(SLOT_PANEL_ALT), 0);
    lv_obj_set_style_bg_color(s_view.btn_reset, lv_color_hex(SLOT_PANEL_ALT), 0);
    show_confirm = (snapshot.state == SLOT_STATE_HIT);
    if (show_confirm) {
        lv_obj_clear_flag(s_view.btn_confirm, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_view.btn_confirm, LV_OBJ_FLAG_HIDDEN);
    }
}

static void start_btn_cb(lv_event_t *event)
{
    (void)event;
    if (bitcoin_service_start_slot(SLOT_MODE_NORMAL) != ESP_OK) {
        slot_apply_snapshot();
        return;
    }
    slot_apply_snapshot();
}

static void pause_btn_cb(lv_event_t *event)
{
    (void)event;
    bitcoin_service_pause_slot();
    slot_apply_snapshot();
}

static void selftest_btn_cb(lv_event_t *event)
{
    (void)event;
    (void)bitcoin_service_start_slot(SLOT_MODE_SELFTEST);
    slot_apply_snapshot();
}

static void reset_btn_cb(lv_event_t *event)
{
    (void)event;
    bitcoin_service_reset_slot_counters();
    slot_apply_snapshot();
}

static void confirm_btn_cb(lv_event_t *event)
{
    (void)event;
    bitcoin_service_acknowledge_hit();
    slot_apply_snapshot();
}

static esp_err_t app_satoshi_slot_init(void) { return ESP_OK; }

static lv_obj_t *app_satoshi_slot_create_root(lv_obj_t *parent)
{
    lv_obj_t *panel;
    lv_obj_t *title;

    memset(&s_view, 0, sizeof(s_view));
    s_view.root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_view.root);
    lv_obj_set_size(s_view.root, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(s_view.root, lv_color_hex(SLOT_BG), 0);
    lv_obj_set_style_bg_opa(s_view.root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_view.root, 0, 0);
    lv_obj_clear_flag(s_view.root, LV_OBJ_FLAG_SCROLLABLE);

    title = lv_label_create(s_view.root);
    lv_label_set_text(title, "Satoshi Slot");
    lv_obj_set_style_text_font(title, ui_font_text_22(), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(SLOT_ACTIVE), 0);
    lv_obj_set_pos(title, SLOT_PAD_X, SLOT_PAD_TOP);

    s_view.state_label = lv_label_create(s_view.root);
    lv_obj_set_width(s_view.state_label, 144);
    lv_obj_set_style_text_font(s_view.state_label, ui_font_text_11(), 0);
    lv_obj_set_style_text_color(s_view.state_label, lv_color_hex(SLOT_MUTED), 0);
    lv_obj_set_style_text_align(s_view.state_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_view.state_label, LV_ALIGN_TOP_RIGHT, -SLOT_PAD_X, SLOT_PAD_TOP + 4);

    s_view.detail_label = lv_label_create(s_view.root);
    lv_obj_set_width(s_view.detail_label, SLOT_LEFT_COL_W);
    lv_obj_set_style_text_font(s_view.detail_label, ui_font_text_11(), 0);
    lv_obj_set_style_text_color(s_view.detail_label, lv_color_hex(SLOT_TEXT), 0);
    lv_label_set_long_mode(s_view.detail_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_view.detail_label, SLOT_PAD_X, SLOT_CONTENT_Y);

    s_view.metrics_label = lv_label_create(s_view.root);
    lv_obj_set_width(s_view.metrics_label, SLOT_LEFT_COL_W);
    lv_obj_set_style_text_font(s_view.metrics_label, ui_font_text_11(), 0);
    lv_obj_set_style_text_color(s_view.metrics_label, lv_color_hex(SLOT_TEXT), 0);
    lv_label_set_long_mode(s_view.metrics_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_pos(s_view.metrics_label, SLOT_PAD_X, SLOT_CONTENT_Y + 44);

    s_view.prefix_label = lv_label_create(s_view.root);
    lv_obj_set_width(s_view.prefix_label, SLOT_LEFT_COL_W);
    lv_obj_set_style_text_font(s_view.prefix_label, ui_font_text_11(), 0);
    lv_obj_set_style_text_color(s_view.prefix_label, lv_color_hex(SLOT_MUTED), 0);
    lv_label_set_long_mode(s_view.prefix_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_pos(s_view.prefix_label, SLOT_PAD_X, SLOT_CONTENT_Y + 60);

    panel = lv_obj_create(s_view.root);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, SLOT_RIGHT_PANEL_W, SLOT_CONTENT_H);
    lv_obj_set_pos(panel, SLOT_RIGHT_PANEL_X, SLOT_CONTENT_Y);
    lv_obj_set_style_bg_color(panel, lv_color_hex(SLOT_PANEL), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 16, 0);
    lv_obj_set_style_pad_all(panel, 12, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    s_view.panel_label = lv_label_create(panel);
    lv_obj_set_width(s_view.panel_label, SLOT_RIGHT_PANEL_W - 24);
    lv_obj_set_style_text_font(s_view.panel_label, ui_font_text_11(), 0);
    lv_obj_set_style_text_color(s_view.panel_label, lv_color_hex(SLOT_TEXT), 0);
    lv_label_set_long_mode(s_view.panel_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_view.panel_label, "");

    s_view.btn_start = create_action_button(s_view.root, "START");
    s_view.btn_pause = create_action_button(s_view.root, "PAUSE");
    s_view.btn_selftest = create_action_button(s_view.root, "SELF-TEST");
    s_view.btn_reset = create_action_button(s_view.root, "RESET");
    s_view.btn_confirm = create_action_button(s_view.root, "CONFIRM");
    lv_obj_set_pos(s_view.btn_start, SLOT_BTN_X0, SLOT_BUTTON_Y);
    lv_obj_set_pos(s_view.btn_pause, SLOT_BTN_X1, SLOT_BUTTON_Y);
    lv_obj_set_size(s_view.btn_selftest, SLOT_BTN_SELFTEST_W, SLOT_BUTTON_H);
    lv_obj_set_pos(s_view.btn_selftest, SLOT_BTN_X2, SLOT_BUTTON_Y);
    lv_obj_set_pos(s_view.btn_reset, SLOT_BTN_X3, SLOT_BUTTON_Y);
    lv_obj_set_size(s_view.btn_confirm, SLOT_BTN_CONFIRM_W, SLOT_BUTTON_H);
    lv_obj_set_pos(s_view.btn_confirm, SLOT_BTN_X4, SLOT_BUTTON_Y);
    lv_obj_add_flag(s_view.btn_confirm, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_event_cb(s_view.btn_start, start_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_view.btn_pause, pause_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_view.btn_selftest, selftest_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_view.btn_reset, reset_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_view.btn_confirm, confirm_btn_cb, LV_EVENT_CLICKED, NULL);

    slot_apply_snapshot();
    return s_view.root;
}

static void app_satoshi_slot_resume(void) { slot_apply_snapshot(); }

static void app_satoshi_slot_handle_event(const app_event_t *event)
{
    if (event == NULL) {
        return;
    }

    switch (event->type) {
    case APP_EVENT_ENTER:
    case APP_EVENT_POWER_CHANGED:
    case APP_EVENT_DATA_BITCOIN:
        slot_apply_snapshot();
        break;
    default:
        break;
    }
}

const app_descriptor_t *app_satoshi_slot_get_descriptor(void)
{
    static const app_descriptor_t descriptor = {
        .id = APP_ID_SATOSHI_SLOT,
        .name = "Satoshi Slot",
        .init = app_satoshi_slot_init,
        .create_root = app_satoshi_slot_create_root,
        .resume = app_satoshi_slot_resume,
        .suspend = NULL,
        .handle_event = app_satoshi_slot_handle_event,
    };

    return &descriptor;
}
