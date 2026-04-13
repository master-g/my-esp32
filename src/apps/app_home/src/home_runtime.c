#include "home_runtime.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "home_internal.h"
#include "home_presenter.h"
#include "screensaver_direct.h"
#include "service_claude.h"
#include "service_home.h"
#include "service_time.h"

static const char *TAG = "home_runtime";

static int64_t home_now_us(void) { return esp_timer_get_time(); }

static void build_model(home_snapshot_t *snapshot, home_present_model_t *model)
{
    home_service_get_snapshot(snapshot);
    home_presenter_build(model, snapshot);
}

static void apply_current_snapshot(home_runtime_t *runtime)
{
    home_snapshot_t snapshot;
    home_present_model_t model;

    if (runtime == NULL || runtime->view.root == NULL) {
        return;
    }

    build_model(&snapshot, &model);
    if (home_screensaver_is_active(&runtime->screensaver)) {
        home_screensaver_apply(&runtime->screensaver, &model);
    } else {
        home_view_apply(&runtime->view, &model);
    }
}

static void stop_unread_timer(home_runtime_t *runtime)
{
    if (runtime->unread_timer != NULL) {
        lv_timer_delete(runtime->unread_timer);
        runtime->unread_timer = NULL;
    }
}

static void unread_auto_clear_cb(lv_timer_t *timer)
{
    home_runtime_t *runtime = lv_timer_get_user_data(timer);

    if (runtime == NULL) {
        return;
    }

    claude_service_mark_read(runtime->unread_seq);
    runtime->unread_timer = NULL;
}

static void schedule_unread_clear(home_runtime_t *runtime, uint32_t seq)
{
    if (runtime == NULL) {
        return;
    }

    runtime->unread_seq = seq;
    if (runtime->unread_timer != NULL) {
        lv_timer_reset(runtime->unread_timer);
        return;
    }

    runtime->unread_timer =
        lv_timer_create(unread_auto_clear_cb, HOME_UNREAD_AUTO_CLEAR_MS, runtime);
    lv_timer_set_repeat_count(runtime->unread_timer, 1);
}

static void enter_screensaver(home_runtime_t *runtime, const home_present_model_t *model)
{
    if (runtime == NULL) {
        return;
    }

    if (home_screensaver_is_active(&runtime->screensaver)) {
        home_screensaver_apply(&runtime->screensaver, model);
        return;
    }

    home_view_on_screensaver_enter(&runtime->view);
    if (runtime->time_refresh_timer != NULL) {
        lv_timer_pause(runtime->time_refresh_timer);
    }
    if (runtime->snapshot_refresh_timer != NULL) {
        lv_timer_pause(runtime->snapshot_refresh_timer);
    }
    home_view_set_hidden(&runtime->view, true);
    home_screensaver_enter(&runtime->screensaver, model);
}

static void exit_screensaver(home_runtime_t *runtime)
{
    if (runtime == NULL || !home_screensaver_is_active(&runtime->screensaver)) {
        return;
    }

    home_screensaver_exit(&runtime->screensaver);
    home_view_set_hidden(&runtime->view, false);
    home_view_on_screensaver_exit(&runtime->view);
    if (runtime->time_refresh_timer != NULL) {
        lv_timer_resume(runtime->time_refresh_timer);
    }
    if (runtime->snapshot_refresh_timer != NULL) {
        lv_timer_resume(runtime->snapshot_refresh_timer);
    }
}

static esp_err_t capture_direct_screensaver(home_runtime_t *runtime, app_screenshot_t *capture)
{
    char time_text[sizeof(runtime->screensaver.fx.time_text)];
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t stride_bytes = 0;
    esp_err_t err;

    if (runtime == NULL || capture == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!home_screensaver_is_active(&runtime->screensaver) ||
        !runtime->screensaver.fx.direct_active) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    memcpy(time_text, runtime->screensaver.fx.time_text, sizeof(time_text));
    time_text[sizeof(time_text) - 1] = '\0';

    err = screensaver_direct_render_snapshot_rgb565(
        runtime->screensaver.fx.time_ms, time_text, (uint16_t *)capture->buffer,
        capture->capacity_bytes, &width, &height, &stride_bytes);
    if (err != ESP_OK) {
        return err;
    }

    capture->data_size = (size_t)stride_bytes * height;
    capture->info.app_id = APP_ID_HOME;
    capture->info.format = APP_SCREENSHOT_FORMAT_RGB565;
    capture->info.source = APP_SCREENSHOT_SOURCE_HOME_DIRECT;
    capture->info.width = width;
    capture->info.height = height;
    capture->info.stride_bytes = stride_bytes;
    return ESP_OK;
}

static void screensaver_touch_exit_cb(void *ctx)
{
    home_runtime_t *runtime = ctx;

    if (runtime == NULL) {
        return;
    }

    home_screensaver_poke_activity(&runtime->screensaver);
    exit_screensaver(runtime);
    apply_current_snapshot(runtime);
}

static void time_refresh_cb(lv_timer_t *timer)
{
    home_runtime_t *runtime = lv_timer_get_user_data(timer);
    char time_text[9];
    uint32_t epoch_s = 0;

    if (runtime == NULL || runtime->view.root == NULL ||
        home_screensaver_is_active(&runtime->screensaver)) {
        return;
    }

    time_service_get_current_text(time_text, sizeof(time_text), &epoch_s);
    home_view_update_time(&runtime->view, time_text);
    if (epoch_s != runtime->last_snapshot_epoch_s) {
        runtime->last_snapshot_epoch_s = epoch_s;
        apply_current_snapshot(runtime);
    }
}

static void snapshot_refresh_cb(lv_timer_t *timer)
{
    home_runtime_t *runtime = lv_timer_get_user_data(timer);

    if (runtime == NULL || runtime->view.root == NULL ||
        home_screensaver_is_active(&runtime->screensaver)) {
        return;
    }

    apply_current_snapshot(runtime);
}

static void settings_btn_cb(lv_event_t *e)
{
    home_runtime_t *runtime = lv_event_get_user_data(e);
    esp_err_t err;

    if (runtime == NULL) {
        return;
    }

    home_screensaver_poke_activity(&runtime->screensaver);
    exit_screensaver(runtime);
    err = app_manager_post_switch_to(APP_ID_SETTINGS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to open settings: %s", esp_err_to_name(err));
    }
}

esp_err_t home_runtime_init(home_runtime_t *runtime)
{
    if (runtime == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(runtime, 0, sizeof(*runtime));
    return ESP_OK;
}

lv_obj_t *home_runtime_create_root(home_runtime_t *runtime, lv_obj_t *parent)
{
    home_snapshot_t snapshot;
    home_present_model_t model;

    if (runtime == NULL) {
        return NULL;
    }

    memset(runtime, 0, sizeof(*runtime));
    home_view_create(&runtime->view, parent);
    home_screensaver_create(&runtime->screensaver, runtime->view.root, screensaver_touch_exit_cb,
                            runtime);
    home_approval_create(&runtime->approval, runtime->view.root);
    if (runtime->view.settings_item != NULL) {
        lv_obj_add_event_cb(runtime->view.settings_item, settings_btn_cb, LV_EVENT_CLICKED,
                            runtime);
    }
    runtime->time_refresh_timer = lv_timer_create(time_refresh_cb, HOME_TIME_REFRESH_MS, runtime);
    runtime->snapshot_refresh_timer =
        lv_timer_create(snapshot_refresh_cb, HOME_SNAPSHOT_REFRESH_MS, runtime);
    home_screensaver_poke_activity(&runtime->screensaver);

    build_model(&snapshot, &model);
    runtime->was_connected = snapshot.claude_connected;
    runtime->last_snapshot_epoch_s = 0;
    home_view_apply(&runtime->view, &model);
    return runtime->view.root;
}

void home_runtime_resume(home_runtime_t *runtime)
{
    home_snapshot_t snapshot;
    home_present_model_t model;

    if (runtime == NULL || runtime->view.root == NULL) {
        return;
    }

    home_screensaver_suspend(&runtime->screensaver);
    home_screensaver_poke_activity(&runtime->screensaver);
    home_view_set_hidden(&runtime->view, false);
    home_view_on_screensaver_exit(&runtime->view);
    if (runtime->time_refresh_timer != NULL) {
        lv_timer_resume(runtime->time_refresh_timer);
    }
    if (runtime->snapshot_refresh_timer != NULL) {
        lv_timer_resume(runtime->snapshot_refresh_timer);
    }

    build_model(&snapshot, &model);
    runtime->was_connected = snapshot.claude_connected;
    runtime->last_snapshot_epoch_s = 0;
    home_view_apply(&runtime->view, &model);
}

void home_runtime_suspend(home_runtime_t *runtime)
{
    if (runtime == NULL) {
        return;
    }

    if (runtime->time_refresh_timer != NULL) {
        lv_timer_pause(runtime->time_refresh_timer);
    }
    if (runtime->snapshot_refresh_timer != NULL) {
        lv_timer_pause(runtime->snapshot_refresh_timer);
    }
    stop_unread_timer(runtime);
    home_view_suspend(&runtime->view);
    home_screensaver_suspend(&runtime->screensaver);
}

void home_runtime_handle_event(home_runtime_t *runtime, const app_event_t *event)
{
    home_snapshot_t snapshot;
    home_present_model_t model;

    if (runtime == NULL || event == NULL) {
        return;
    }

    switch (event->type) {
    case APP_EVENT_TICK_1S:
        if (home_screensaver_should_enter(&runtime->screensaver, home_now_us())) {
            build_model(&snapshot, &model);
            enter_screensaver(runtime, &model);
        } else {
            apply_current_snapshot(runtime);
        }
        break;
    case APP_EVENT_POWER_CHANGED:
    case APP_EVENT_NET_CHANGED:
    case APP_EVENT_DATA_WEATHER:
        apply_current_snapshot(runtime);
        break;
    case APP_EVENT_DATA_CLAUDE:
        build_model(&snapshot, &model);
        home_screensaver_poke_activity(&runtime->screensaver);
        exit_screensaver(runtime);
        home_approval_on_connection_changed(&runtime->approval, runtime->was_connected,
                                            snapshot.claude_connected);
        runtime->was_connected = snapshot.claude_connected;
        if (snapshot.claude_unread) {
            claude_snapshot_t claude_snapshot;

            claude_service_get_snapshot(&claude_snapshot);
            schedule_unread_clear(runtime, claude_snapshot.seq);
        } else {
            stop_unread_timer(runtime);
        }
        home_view_apply(&runtime->view, &model);
        break;
    case APP_EVENT_PERMISSION_REQUEST:
        home_screensaver_poke_activity(&runtime->screensaver);
        exit_screensaver(runtime);
        home_approval_show_pending(&runtime->approval);
        break;
    case APP_EVENT_PERMISSION_DISMISS:
        home_approval_hide(&runtime->approval);
        break;
    default:
        break;
    }
}

esp_err_t home_runtime_handle_control(home_runtime_t *runtime, app_control_type_t type,
                                      const void *payload)
{
    home_snapshot_t snapshot;
    home_present_model_t model;

    if (runtime == NULL || runtime->view.root == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    switch (type) {
    case APP_CONTROL_HOME_SCREENSAVER: {
        const app_control_home_screensaver_t *control = payload;

        ESP_RETURN_ON_FALSE(control != NULL, ESP_ERR_INVALID_ARG, "home_runtime",
                            "screensaver control is required");
        build_model(&snapshot, &model);
        if (control->enabled) {
            enter_screensaver(runtime, &model);
        } else {
            home_screensaver_poke_activity(&runtime->screensaver);
            exit_screensaver(runtime);
            home_view_apply(&runtime->view, &model);
        }
        return ESP_OK;
    }
    case APP_CONTROL_CAPTURE_SCREENSHOT:
        return capture_direct_screensaver(runtime, (app_screenshot_t *)payload);
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}
