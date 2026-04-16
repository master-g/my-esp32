#include "home_screensaver.h"

#include <string.h>

#include "bsp_board_config.h"
#include "bsp_display.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "generated/departure_mono_55.h"
#include "home_internal.h"
#include "screensaver_direct.h"
#include "screensaver_renderer.h"
#include "service_home.h"

#define TAG "home_screensaver"

static int64_t home_now_us(void) { return esp_timer_get_time(); }

void home_screensaver_mute_perf_logs(home_screensaver_t *screensaver, uint32_t duration_ms)
{
    if (screensaver == NULL) {
        return;
    }

    screensaver->fx.perf_log_muted_until_us = home_now_us() + ((int64_t)duration_ms * 1000LL);
}

static uint16_t home_avg_us_to_ms_x10(uint64_t total_us, uint32_t samples)
{
    if (samples == 0) {
        return 0;
    }

    return (uint16_t)((total_us * 10ULL + ((uint64_t)samples * 500ULL)) /
                      ((uint64_t)samples * 1000ULL));
}

static void set_overlay_children_hidden(home_screensaver_t *screensaver, bool hidden)
{
    lv_obj_t *objects[] = {
        screensaver->fx.image,
        screensaver->time_label,
    };
    size_t i;

    for (i = 0; i < sizeof(objects) / sizeof(objects[0]); ++i) {
        if (objects[i] == NULL) {
            continue;
        }

        if (hidden) {
            lv_obj_add_flag(objects[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(objects[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void update_time_label(home_screensaver_t *screensaver, const char *time_text)
{
    if (screensaver == NULL || time_text == NULL) {
        return;
    }

    memcpy(screensaver->fx.time_text, time_text, sizeof(screensaver->fx.time_text));
    if (!screensaver->fx.direct_active && screensaver->time_label != NULL) {
        lv_label_set_text(screensaver->time_label, time_text);
    }
}

static void render_background(home_screensaver_t *screensaver)
{
    lv_draw_buf_t *draw_buf;
    lv_color32_t *pixels;
    uint32_t stride_px;

    if (screensaver->fx.direct_active) {
        esp_err_t err =
            screensaver_direct_render_and_push(screensaver->fx.time_ms, screensaver->fx.time_text);
        if (err != ESP_OK && !screensaver->fx.direct_push_warned) {
            ESP_LOGW(TAG, "screensaver direct push failed: %s", esp_err_to_name(err));
            screensaver->fx.direct_push_warned = true;
        } else if (err == ESP_OK) {
            screensaver->fx.direct_push_warned = false;
        }
        return;
    }

    if (screensaver->fx.canvas == NULL || screensaver->fx.buf == NULL) {
        return;
    }

    draw_buf = lv_canvas_get_draw_buf(screensaver->fx.canvas);
    if (draw_buf == NULL || draw_buf->data == NULL) {
        return;
    }

    pixels = (lv_color32_t *)draw_buf->data;
    stride_px = draw_buf->header.stride / sizeof(lv_color32_t);
    screensaver_renderer_render(pixels, stride_px, screensaver->fx.time_ms);

    if (screensaver->fx.image != NULL) {
        lv_obj_invalidate(screensaver->fx.image);
    }
}

static void refresh_time_from_services(home_screensaver_t *screensaver)
{
    home_snapshot_t snapshot;
    home_present_model_t model;

    home_service_get_snapshot(&snapshot);
    home_presenter_build(&model, &snapshot);
    update_time_label(screensaver, model.screensaver_time_text);
}

static void tick(home_screensaver_t *screensaver)
{
    int64_t frame_start_us;
    int64_t frame_end_us;
    uint32_t interval_us = 0;
    uint32_t render_us;
    int64_t window_elapsed_us;

    if (screensaver == NULL || !screensaver->active) {
        return;
    }

    frame_start_us = home_now_us();
    if (screensaver->fx.prev_frame_start_us != 0 &&
        frame_start_us > screensaver->fx.prev_frame_start_us) {
        interval_us = (uint32_t)(frame_start_us - screensaver->fx.prev_frame_start_us);
    }
    screensaver->fx.prev_frame_start_us = frame_start_us;
    if (screensaver->fx.time_origin_us == 0) {
        screensaver->fx.time_origin_us = frame_start_us;
    }
    screensaver->fx.time_ms =
        (uint32_t)((frame_start_us - screensaver->fx.time_origin_us) / 1000LL);

    render_background(screensaver);

    frame_end_us = home_now_us();
    render_us = (uint32_t)(frame_end_us - frame_start_us);

    if (screensaver->fx.perf_window_start_us == 0) {
        screensaver->fx.perf_window_start_us = frame_start_us;
    }

    screensaver->fx.perf_frames++;
    screensaver->fx.perf_render_total_us += render_us;
    if (interval_us != 0) {
        screensaver->fx.perf_interval_total_us += interval_us;
        screensaver->fx.perf_interval_samples++;
    }

    window_elapsed_us = frame_end_us - screensaver->fx.perf_window_start_us;
    if (window_elapsed_us >= 500000) {
        if (screensaver->fx.direct_active) {
            screensaver_direct_perf_snapshot_t direct_perf = {0};

            screensaver->fx.fps_x10 =
                (uint16_t)((screensaver->fx.perf_frames * 10000000LL + window_elapsed_us / 2) /
                           window_elapsed_us);
            screensaver->fx.interval_ms_x10 = home_avg_us_to_ms_x10(
                screensaver->fx.perf_interval_total_us, screensaver->fx.perf_interval_samples);
            screensaver->fx.render_ms_x10 = home_avg_us_to_ms_x10(
                screensaver->fx.perf_render_total_us, screensaver->fx.perf_frames);
            screensaver_direct_get_perf_snapshot(&direct_perf);
            if (frame_end_us >= screensaver->fx.perf_log_muted_until_us) {
                ESP_LOGI(TAG,
                         "screensaver_perf fps=%u.%u int=%u.%u frm=%u.%u cmp=%u.%u txt=%u.%u "
                         "wait=%u.%u push=%u.%u",
                         screensaver->fx.fps_x10 / 10, screensaver->fx.fps_x10 % 10,
                         screensaver->fx.interval_ms_x10 / 10, screensaver->fx.interval_ms_x10 % 10,
                         screensaver->fx.render_ms_x10 / 10, screensaver->fx.render_ms_x10 % 10,
                         direct_perf.compose_ms_x10 / 10, direct_perf.compose_ms_x10 % 10,
                         direct_perf.text_ms_x10 / 10, direct_perf.text_ms_x10 % 10,
                         direct_perf.wait_ms_x10 / 10, direct_perf.wait_ms_x10 % 10,
                         direct_perf.push_ms_x10 / 10, direct_perf.push_ms_x10 % 10);
            }
        }

        screensaver->fx.perf_frames = 0;
        screensaver->fx.perf_interval_samples = 0;
        screensaver->fx.perf_render_total_us = 0;
        screensaver->fx.perf_interval_total_us = 0;
        screensaver->fx.perf_window_start_us = frame_end_us;
    }

    if (screensaver->fx.last_time_refresh_us == 0 ||
        (frame_end_us - screensaver->fx.last_time_refresh_us) >= 1000000) {
        screensaver->fx.last_time_refresh_us = frame_end_us;
        refresh_time_from_services(screensaver);
    }
}

static void fx_timer_cb(lv_timer_t *timer)
{
    home_screensaver_t *screensaver = lv_timer_get_user_data(timer);

    if (screensaver == NULL || screensaver->fx.direct_active) {
        return;
    }

    tick(screensaver);
}

static void direct_task(void *arg)
{
    home_screensaver_t *screensaver = arg;
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        if (screensaver->fx.direct_stop_requested) {
            break;
        }

        tick(screensaver);
        if (screensaver->fx.direct_stop_requested) {
            break;
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(HOME_SCREENSAVER_DIRECT_PERIOD_MS));
    }

    screensaver->fx.direct_task = NULL;
    if (screensaver->fx.direct_task_done != NULL) {
        xSemaphoreGive(screensaver->fx.direct_task_done);
    }

    vTaskDelete(NULL);
}

static bool start_direct_task(home_screensaver_t *screensaver)
{
    BaseType_t ret;

    if (!screensaver->fx.direct_active) {
        return false;
    }
    if (screensaver->fx.direct_task != NULL) {
        return true;
    }

    if (screensaver->fx.direct_task_done == NULL) {
        screensaver->fx.direct_task_done = xSemaphoreCreateBinary();
        if (screensaver->fx.direct_task_done == NULL) {
            ESP_LOGW(TAG, "screensaver direct task semaphore alloc failed");
            return false;
        }
    }

    while (xSemaphoreTake(screensaver->fx.direct_task_done, 0) == pdTRUE) {
    }

    screensaver->fx.direct_stop_requested = false;
    ret = xTaskCreatePinnedToCore(direct_task, "home_ss_direct", HOME_SCREENSAVER_TASK_STACK_SIZE,
                                  screensaver, HOME_SCREENSAVER_TASK_PRIORITY,
                                  &screensaver->fx.direct_task, HOME_SCREENSAVER_TASK_CORE);
    if (ret != pdPASS) {
        screensaver->fx.direct_task = NULL;
        ESP_LOGW(TAG, "screensaver direct task create failed; using LVGL timer");
        return false;
    }

    return true;
}

static void stop_direct_task(home_screensaver_t *screensaver)
{
    if (screensaver->fx.direct_task == NULL) {
        return;
    }

    screensaver->fx.direct_stop_requested = true;
    if (screensaver->fx.direct_task_done == NULL) {
        return;
    }

    if (xSemaphoreTake(screensaver->fx.direct_task_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "screensaver direct task stop timeout");
        return;
    }

    while (xSemaphoreTake(screensaver->fx.direct_task_done, 0) == pdTRUE) {
    }
}

static void start_fx(home_screensaver_t *screensaver)
{
    if (screensaver->fx.timer == NULL) {
        return;
    }

    screensaver->fx.fps_x10 = 0;
    screensaver->fx.interval_ms_x10 = 0;
    screensaver->fx.render_ms_x10 = 0;
    screensaver->fx.perf_frames = 0;
    screensaver->fx.perf_interval_samples = 0;
    screensaver->fx.perf_render_total_us = 0;
    screensaver->fx.perf_interval_total_us = 0;
    screensaver->fx.perf_window_start_us = 0;
    screensaver->fx.prev_frame_start_us = 0;
    screensaver->fx.time_origin_us = 0;
    screensaver->fx.time_ms = 0;
    screensaver->fx.last_time_refresh_us = 0;
    screensaver->fx.direct_push_warned = false;
    screensaver->fx.direct_stop_requested = false;
    screensaver->fx.direct_active =
        screensaver_direct_is_ready() && bsp_display_begin_direct_mode();
    if (screensaver_direct_is_ready() && !screensaver->fx.direct_active) {
        ESP_LOGW(TAG, "screensaver direct mode unavailable; falling back to LVGL path");
    }
    if (screensaver->fx.direct_active) {
        screensaver_direct_reset();
    }
    set_overlay_children_hidden(screensaver, screensaver->fx.direct_active);
    if (screensaver->fx.direct_active) {
        if (!start_direct_task(screensaver)) {
            bsp_display_end_direct_mode();
            screensaver_direct_restore_background();
            screensaver->fx.direct_active = false;
            set_overlay_children_hidden(screensaver, false);
            render_background(screensaver);
            lv_timer_resume(screensaver->fx.timer);
        }
    } else {
        render_background(screensaver);
        lv_timer_resume(screensaver->fx.timer);
    }
}

static void stop_fx(home_screensaver_t *screensaver)
{
    if (screensaver->fx.timer != NULL) {
        lv_timer_pause(screensaver->fx.timer);
    }

    if (!screensaver->fx.direct_active) {
        return;
    }

    stop_direct_task(screensaver);
    if (!screensaver_direct_wait_idle(1000)) {
        ESP_LOGW(TAG, "screensaver direct pipeline idle wait timeout");
    }
    bsp_display_end_direct_mode();
    screensaver_direct_restore_background();
    screensaver->fx.direct_active = false;
    screensaver->fx.direct_stop_requested = false;
    set_overlay_children_hidden(screensaver, false);
    lv_obj_invalidate(lv_screen_active());
}

static void touch_cb(lv_event_t *e)
{
    home_screensaver_t *screensaver = lv_event_get_user_data(e);

    if (screensaver == NULL || !screensaver->active || screensaver->touch_cb == NULL) {
        return;
    }

    screensaver->touch_cb(screensaver->touch_cb_ctx);
}

void home_screensaver_create(home_screensaver_t *screensaver, lv_obj_t *root,
                             home_screensaver_touch_cb_t touch_cb_fn, void *touch_cb_ctx)
{
    size_t fx_buf_size = HOME_SCREENSAVER_FX_W * HOME_SCREENSAVER_FX_H * sizeof(lv_color32_t);

    if (screensaver == NULL || root == NULL) {
        return;
    }

    memset(screensaver, 0, sizeof(*screensaver));
    screensaver->last_activity_us = home_now_us();
    screensaver->touch_cb = touch_cb_fn;
    screensaver->touch_cb_ctx = touch_cb_ctx;

    screensaver->overlay = lv_obj_create(root);
    lv_obj_remove_style_all(screensaver->overlay);
    lv_obj_set_size(screensaver->overlay, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(screensaver->overlay, lv_color_hex(HOME_BG_BASE_COLOR), 0);
    lv_obj_set_style_bg_opa(screensaver->overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(screensaver->overlay,
                    LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(screensaver->overlay, LV_ALIGN_TOP_LEFT, -16, -16);
    lv_obj_add_event_cb(screensaver->overlay, touch_cb, LV_EVENT_PRESSED, screensaver);

    screensaver->fx.buf = heap_caps_malloc(fx_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (screensaver->fx.buf == NULL) {
        screensaver->fx.buf = heap_caps_malloc(fx_buf_size, MALLOC_CAP_8BIT);
    }

    if (screensaver->fx.buf != NULL) {
        screensaver->fx.canvas = lv_canvas_create(screensaver->overlay);
        lv_canvas_set_buffer(screensaver->fx.canvas, screensaver->fx.buf, HOME_SCREENSAVER_FX_W,
                             HOME_SCREENSAVER_FX_H, LV_COLOR_FORMAT_ARGB8888);
        lv_obj_add_flag(screensaver->fx.canvas, LV_OBJ_FLAG_HIDDEN);

        screensaver->fx.image = lv_image_create(screensaver->overlay);
        lv_image_set_src(screensaver->fx.image, lv_canvas_get_image(screensaver->fx.canvas));
        lv_image_set_scale(screensaver->fx.image, HOME_SCREENSAVER_FX_SCALE);
        lv_obj_center(screensaver->fx.image);

        screensaver_renderer_init(HOME_SCREENSAVER_FX_W, HOME_SCREENSAVER_FX_H);
        if (!screensaver_direct_init()) {
            ESP_LOGW(TAG, "screensaver direct buffer alloc failed; using LVGL fallback");
        }

        memcpy(screensaver->fx.time_text, "--:--", sizeof(screensaver->fx.time_text));
        render_background(screensaver);

        screensaver->fx.timer =
            lv_timer_create(fx_timer_cb, HOME_SCREENSAVER_FX_PERIOD_MS, screensaver);
        lv_timer_pause(screensaver->fx.timer);
    } else {
        ESP_LOGW(TAG, "screensaver fx buffer allocation failed; using static background");
    }

    screensaver->time_label = lv_label_create(screensaver->overlay);
    lv_obj_set_style_text_font(screensaver->time_label, &departure_mono_55, 0);
    lv_obj_set_style_text_color(screensaver->time_label, lv_color_hex(HOME_SCREENSAVER_TIME_COLOR),
                                0);
    lv_obj_set_style_text_letter_space(screensaver->time_label, HOME_SCREENSAVER_TIME_LETTER_SPACE,
                                       0);
    lv_obj_center(screensaver->time_label);
    lv_label_set_text_static(screensaver->time_label, "--:--");
}

void home_screensaver_enter(home_screensaver_t *screensaver, const home_present_model_t *model)
{
    if (screensaver == NULL) {
        return;
    }

    screensaver->active = true;
    if (model != NULL) {
        update_time_label(screensaver, model->screensaver_time_text);
    }
    if (screensaver->overlay != NULL) {
        lv_obj_clear_flag(screensaver->overlay, LV_OBJ_FLAG_HIDDEN);
    }
    start_fx(screensaver);
}

void home_screensaver_exit(home_screensaver_t *screensaver)
{
    if (screensaver == NULL || !screensaver->active) {
        return;
    }

    screensaver->active = false;
    stop_fx(screensaver);
    if (screensaver->overlay != NULL) {
        lv_obj_add_flag(screensaver->overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

void home_screensaver_suspend(home_screensaver_t *screensaver)
{
    if (screensaver == NULL) {
        return;
    }

    screensaver->active = false;
    stop_fx(screensaver);
    if (screensaver->overlay != NULL) {
        lv_obj_add_flag(screensaver->overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

void home_screensaver_apply(home_screensaver_t *screensaver, const home_present_model_t *model)
{
    if (screensaver == NULL || model == NULL) {
        return;
    }

    update_time_label(screensaver, model->screensaver_time_text);
}

void home_screensaver_poke_activity(home_screensaver_t *screensaver)
{
    if (screensaver != NULL) {
        screensaver->last_activity_us = home_now_us();
    }
}

bool home_screensaver_should_enter(const home_screensaver_t *screensaver, int64_t now_us)
{
    return screensaver != NULL && !screensaver->active &&
           (now_us - screensaver->last_activity_us) >= HOME_SCREENSAVER_IDLE_US;
}

bool home_screensaver_is_active(const home_screensaver_t *screensaver)
{
    return screensaver != NULL && screensaver->active;
}
