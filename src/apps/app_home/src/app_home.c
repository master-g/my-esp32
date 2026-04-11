#include "app_home.h"

#include <stdio.h>
#include <string.h>

#include "bsp_board.h"
#include "bsp_board_config.h"
#include "bsp_display.h"
#include "device_link.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "generated/app_home_status_font.h"
#include "generated/departure_mono_55.h"
#include "generated/sprite_frames.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "screensaver_balatro.h"
#include "screensaver_direct.h"
#include "service_claude.h"
#include "service_home.h"
#include "service_time.h"
#include "ui_fonts.h"

#define TAG "app_home"

#define HOME_STATUS_BAR_HEIGHT 20
#define HOME_STATUS_BAR_WIDTH 80
#define HOME_STATUS_ICON_GAP 6
#define HOME_STATUS_ICON_SIZE 16
#define HOME_STATUS_ITEM_BOX_SIZE (HOME_STATUS_ICON_SIZE + 4)
#define HOME_STATUS_BAR_Y 4
#define HOME_TIME_Y 33
#define HOME_DATE_Y 88
#define HOME_WIFI_ONLINE_COLOR 0xffffff
#define HOME_WIFI_CONNECTING_COLOR 0xf3c76d
#define HOME_ICON_MUTED_COLOR 0x5f6b72
#define HOME_CLAUDE_ACTIVE_COLOR 0xffffff
#define HOME_CLAUDE_UNREAD_DOT_COLOR 0xf06c41
#define HOME_WIFI_CONNECTING_DOT_COLOR 0xf3c76d
#define HOME_WEATHER_ROW_HEIGHT 20
#define HOME_WEATHER_ICON_GAP 8
#define HOME_WEATHER_ICON_Y_OFFSET 0
#define HOME_WEATHER_TEXT_Y_OFFSET 3
#define HOME_WEATHER_TEXT_COLOR 0xe3edf2
#define HOME_WEATHER_MUTED_COLOR 0x8da0ab
#define HOME_BG_BASE_COLOR 0x0f1418
#define HOME_BUBBLE_BG_COLOR 0x1e2830
#define HOME_BUBBLE_TEXT_COLOR 0xe3edf2
#define HOME_BUBBLE_MAX_W 220
#define HOME_BUBBLE_PAD_H 6
#define HOME_BUBBLE_PAD_V 4
#define HOME_BUBBLE_RADIUS 8
#define HOME_BUBBLE_FADE_MS 5000
#define HOME_UNREAD_AUTO_CLEAR_MS 5000
#define HOME_SCREENSAVER_IDLE_US (30LL * 60 * 1000000LL)
#define HOME_SCREENSAVER_TIME_COLOR 0xf3f8ff
#define HOME_SCREENSAVER_TIME_LETTER_SPACE 3
#define HOME_SCREENSAVER_FX_W 160
#define HOME_SCREENSAVER_FX_H 43
#define HOME_SCREENSAVER_FX_SCALE 1024U
#define HOME_SCREENSAVER_FX_PERIOD_MS 33
#define HOME_SCREENSAVER_DIRECT_PERIOD_MS 1
#define HOME_TIME_REFRESH_MS 200
#define HOME_SCREENSAVER_TASK_STACK_SIZE (6 * 1024)
#define HOME_SCREENSAVER_TASK_PRIORITY 2
#define HOME_SCREENSAVER_TASK_CORE 1

#define HOME_LEFT_HALF_W 280
#define HOME_SPRITE_SCALE 512 /* 256 = 1x, 512 = 2x */

#define APPROVE_BG_COLOR 0x1a2332
#define APPROVE_BTN_H 40
#define APPROVE_BTN_GAP 8
#define APPROVE_ALLOW_COLOR 0x2d8c4e
#define APPROVE_DENY_COLOR 0x8c3030
#define APPROVE_YOLO_COLOR 0x6b5c2e
#define APPROVE_INFO_H 40

typedef enum {
    SPRITE_STATE_IDLE = 0,
    SPRITE_STATE_WORKING,
    SPRITE_STATE_WAITING,
    SPRITE_STATE_SLEEPING,
    SPRITE_STATE_COUNT,
} sprite_state_t;

typedef struct {
    const lv_image_dsc_t *frames;
    uint8_t num_frames;
    uint16_t period_ms;
} sprite_anim_def_t;

static const sprite_anim_def_t s_sprite_anims[SPRITE_STATE_COUNT] = {
    [SPRITE_STATE_IDLE] = {sprite_idle_frames, SPRITE_FRAMES_PER_STATE, 333},
    [SPRITE_STATE_WORKING] = {sprite_working_frames, SPRITE_FRAMES_PER_STATE, 250},
    [SPRITE_STATE_WAITING] = {sprite_waiting_frames, SPRITE_FRAMES_PER_STATE, 333},
    [SPRITE_STATE_SLEEPING] = {sprite_sleeping_frames, SPRITE_FRAMES_PER_STATE, 500},
};

typedef struct {
    lv_obj_t *root;
    lv_obj_t *screensaver_overlay;
    lv_obj_t *screensaver_time_label;
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
    lv_obj_t *sprite_img;
    lv_obj_t *bubble_box;
    lv_obj_t *bubble_label;
    /* approval overlay */
    lv_obj_t *approval_overlay;
    lv_obj_t *approval_tool_label;
    lv_obj_t *approval_desc_label;
    lv_obj_t *approval_btn_allow;
    lv_obj_t *approval_btn_deny;
    lv_obj_t *approval_btn_yolo;
} app_home_view_t;

typedef struct {
    sprite_state_t state;
    const sprite_anim_def_t *anim;
    uint8_t frame_idx;
    lv_timer_t *timer;
} sprite_ctx_t;

typedef struct {
    lv_obj_t *canvas;
    lv_obj_t *image;
    lv_timer_t *timer;
    uint8_t *buf;
    bool direct_active;
    uint32_t time_ms;
    uint8_t frame_count;
    uint16_t fps_x10;
    uint16_t interval_ms_x10;
    uint16_t render_ms_x10;
    uint16_t perf_frames;
    uint16_t perf_interval_samples;
    uint32_t perf_render_total_us;
    uint64_t perf_interval_total_us;
    int64_t perf_window_start_us;
    int64_t prev_frame_start_us;
    int64_t time_origin_us;
    int64_t last_time_refresh_us;
    bool direct_push_warned;
    volatile bool direct_stop_requested;
    TaskHandle_t direct_task;
    SemaphoreHandle_t direct_task_done;
    char time_text[6];
} screensaver_fx_t;

typedef enum {
    HOME_DISPLAY_NORMAL = 0,
    HOME_DISPLAY_SCREENSAVER,
} home_display_mode_t;

static app_home_view_t s_view;
static sprite_ctx_t s_sprite;
static screensaver_fx_t s_screensaver_fx;
static lv_timer_t *s_bubble_timer;
static bool s_bubble_dismissed;
static lv_timer_t *s_unread_timer;
static uint32_t s_unread_seq;
static bool s_was_connected;
static int64_t s_last_claude_activity_us;
static home_display_mode_t s_display_mode;
static lv_timer_t *s_time_refresh_timer;

static void refresh_view(void);
static int64_t home_now_us(void);
static void update_screensaver_time_label(const home_snapshot_t *snapshot);
static void update_screensaver_perf_label(void);
static void screensaver_tick(void);

static bool start_direct_screensaver_task(void);
static void stop_direct_screensaver_task(void);

static uint16_t home_avg_us_to_ms_x10(uint64_t total_us, uint32_t samples)
{
    if (samples == 0) {
        return 0;
    }

    return (uint16_t)((total_us * 10ULL + ((uint64_t)samples * 500ULL)) /
                      ((uint64_t)samples * 1000ULL));
}

static void set_screensaver_overlay_children_hidden(bool hidden)
{
    lv_obj_t *objs[] = {
        s_screensaver_fx.image,
        s_view.screensaver_time_label,
    };

    for (size_t i = 0; i < sizeof(objs) / sizeof(objs[0]); i++) {
        if (objs[i] == NULL) {
            continue;
        }

        if (hidden) {
            lv_obj_add_flag(objs[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(objs[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static sprite_state_t map_run_state(claude_run_state_t rs, bool connected)
{
    if (!connected) {
        return SPRITE_STATE_SLEEPING;
    }

    switch (rs) {
    case CLAUDE_RUN_PROCESSING:
    case CLAUDE_RUN_RUNNING_TOOL:
    case CLAUDE_RUN_COMPACTING:
        return SPRITE_STATE_WORKING;
    case CLAUDE_RUN_WAITING_FOR_INPUT:
        return SPRITE_STATE_WAITING;
    case CLAUDE_RUN_ENDED:
        return SPRITE_STATE_SLEEPING;
    default:
        return SPRITE_STATE_IDLE;
    }
}

static void sprite_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_view.sprite_img == NULL || s_sprite.anim == NULL) {
        return;
    }

    s_sprite.frame_idx = (s_sprite.frame_idx + 1) % s_sprite.anim->num_frames;
    lv_image_set_src(s_view.sprite_img, &s_sprite.anim->frames[s_sprite.frame_idx]);
}

static int64_t home_now_us(void) { return esp_timer_get_time(); }

static void time_refresh_cb(lv_timer_t *t)
{
    (void)t;
    if (s_display_mode != HOME_DISPLAY_NORMAL || s_view.time_label == NULL) {
        return;
    }

    time_service_refresh_now();
    home_service_refresh_snapshot();
    home_snapshot_t snap;
    home_service_get_snapshot(&snap);
    lv_label_set_text(s_view.time_label, snap.time_text);
}

static void reset_screensaver_idle_deadline(void) { s_last_claude_activity_us = home_now_us(); }

static void update_screensaver_perf_label(void)
{
    /* Perf remains available through serial logs; the on-screen debug overlay is removed. */
}

static void render_screensaver_background(void)
{
    lv_draw_buf_t *draw_buf;
    lv_color32_t *pixels;
    uint32_t stride_px;

    if (s_screensaver_fx.direct_active) {
        esp_err_t err = screensaver_direct_render_and_push(s_screensaver_fx.time_ms,
                                                           s_screensaver_fx.time_text);
        if (err != ESP_OK && !s_screensaver_fx.direct_push_warned) {
            ESP_LOGW(TAG, "screensaver direct push failed: %s", esp_err_to_name(err));
            s_screensaver_fx.direct_push_warned = true;
        } else if (err == ESP_OK) {
            s_screensaver_fx.direct_push_warned = false;
        }
        return;
    }

    if (s_screensaver_fx.canvas == NULL || s_screensaver_fx.buf == NULL) {
        return;
    }

    draw_buf = lv_canvas_get_draw_buf(s_screensaver_fx.canvas);
    if (draw_buf == NULL || draw_buf->data == NULL) {
        return;
    }

    pixels = (lv_color32_t *)draw_buf->data;
    stride_px = draw_buf->header.stride / sizeof(lv_color32_t);

    balatro_render(pixels, stride_px, s_screensaver_fx.time_ms);

    if (s_screensaver_fx.image != NULL) {
        lv_obj_invalidate(s_screensaver_fx.image);
    }
}

static void screensaver_tick(void)
{
    int64_t frame_start_us;
    int64_t frame_end_us;
    uint32_t interval_us = 0;
    uint32_t render_us;
    int64_t window_elapsed_us;

    if (s_display_mode != HOME_DISPLAY_SCREENSAVER) {
        return;
    }

    frame_start_us = home_now_us();
    if (s_screensaver_fx.prev_frame_start_us != 0 &&
        frame_start_us > s_screensaver_fx.prev_frame_start_us) {
        interval_us = (uint32_t)(frame_start_us - s_screensaver_fx.prev_frame_start_us);
    }
    s_screensaver_fx.prev_frame_start_us = frame_start_us;
    if (s_screensaver_fx.time_origin_us == 0) {
        s_screensaver_fx.time_origin_us = frame_start_us;
    }
    s_screensaver_fx.time_ms =
        (uint32_t)((frame_start_us - s_screensaver_fx.time_origin_us) / 1000LL);
    render_screensaver_background();
    frame_end_us = home_now_us();
    render_us = (uint32_t)(frame_end_us - frame_start_us);

    if (s_screensaver_fx.perf_window_start_us == 0) {
        s_screensaver_fx.perf_window_start_us = frame_start_us;
    }

    s_screensaver_fx.perf_frames++;
    s_screensaver_fx.perf_render_total_us += render_us;
    if (interval_us != 0) {
        s_screensaver_fx.perf_interval_total_us += interval_us;
        s_screensaver_fx.perf_interval_samples++;
    }

    window_elapsed_us = frame_end_us - s_screensaver_fx.perf_window_start_us;
    if (window_elapsed_us >= 500000) {
        screensaver_direct_perf_snapshot_t direct_perf = {0};

        s_screensaver_fx.fps_x10 =
            (uint16_t)((s_screensaver_fx.perf_frames * 10000000LL + window_elapsed_us / 2) /
                       window_elapsed_us);
        s_screensaver_fx.interval_ms_x10 = home_avg_us_to_ms_x10(
            s_screensaver_fx.perf_interval_total_us, s_screensaver_fx.perf_interval_samples);
        s_screensaver_fx.render_ms_x10 = home_avg_us_to_ms_x10(
            s_screensaver_fx.perf_render_total_us, s_screensaver_fx.perf_frames);
        update_screensaver_perf_label();
        if (s_screensaver_fx.direct_active) {
            screensaver_direct_get_perf_snapshot(&direct_perf);
            ESP_LOGI(TAG,
                     "screensaver_perf fps=%u.%u int=%u.%u frm=%u.%u cmp=%u.%u txt=%u.%u "
                     "wait=%u.%u push=%u.%u",
                     s_screensaver_fx.fps_x10 / 10, s_screensaver_fx.fps_x10 % 10,
                     s_screensaver_fx.interval_ms_x10 / 10, s_screensaver_fx.interval_ms_x10 % 10,
                     s_screensaver_fx.render_ms_x10 / 10, s_screensaver_fx.render_ms_x10 % 10,
                     direct_perf.compose_ms_x10 / 10, direct_perf.compose_ms_x10 % 10,
                     direct_perf.text_ms_x10 / 10, direct_perf.text_ms_x10 % 10,
                     direct_perf.wait_ms_x10 / 10, direct_perf.wait_ms_x10 % 10,
                     direct_perf.push_ms_x10 / 10, direct_perf.push_ms_x10 % 10);
        }
        s_screensaver_fx.perf_frames = 0;
        s_screensaver_fx.perf_interval_samples = 0;
        s_screensaver_fx.perf_render_total_us = 0;
        s_screensaver_fx.perf_interval_total_us = 0;
        s_screensaver_fx.perf_window_start_us = frame_end_us;
    }

    if (s_screensaver_fx.last_time_refresh_us == 0 ||
        (frame_end_us - s_screensaver_fx.last_time_refresh_us) >= 1000000) {
        s_screensaver_fx.last_time_refresh_us = frame_end_us;
        time_service_refresh_now();
        home_service_refresh_snapshot();
        home_snapshot_t snap;
        home_service_get_snapshot(&snap);
        update_screensaver_time_label(&snap);
    }
}

static void screensaver_fx_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_screensaver_fx.direct_active) {
        return;
    }

    screensaver_tick();
}

static void screensaver_direct_task(void *arg)
{
    TickType_t last_wake = xTaskGetTickCount();

    (void)arg;
    for (;;) {
        if (s_screensaver_fx.direct_stop_requested) {
            break;
        }

        screensaver_tick();
        if (s_screensaver_fx.direct_stop_requested) {
            break;
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(HOME_SCREENSAVER_DIRECT_PERIOD_MS));
    }

    s_screensaver_fx.direct_task = NULL;
    if (s_screensaver_fx.direct_task_done != NULL) {
        xSemaphoreGive(s_screensaver_fx.direct_task_done);
    }

    vTaskDelete(NULL);
}

static bool start_direct_screensaver_task(void)
{
    BaseType_t ret;

    if (!s_screensaver_fx.direct_active) {
        return false;
    }
    if (s_screensaver_fx.direct_task != NULL) {
        return true;
    }

    if (s_screensaver_fx.direct_task_done == NULL) {
        s_screensaver_fx.direct_task_done = xSemaphoreCreateBinary();
        if (s_screensaver_fx.direct_task_done == NULL) {
            ESP_LOGW(TAG, "screensaver direct task semaphore alloc failed");
            return false;
        }
    }

    while (xSemaphoreTake(s_screensaver_fx.direct_task_done, 0) == pdTRUE) {
    }

    s_screensaver_fx.direct_stop_requested = false;
    ret = xTaskCreatePinnedToCore(
        screensaver_direct_task, "home_ss_direct", HOME_SCREENSAVER_TASK_STACK_SIZE, NULL,
        HOME_SCREENSAVER_TASK_PRIORITY, &s_screensaver_fx.direct_task, HOME_SCREENSAVER_TASK_CORE);
    if (ret != pdPASS) {
        s_screensaver_fx.direct_task = NULL;
        ESP_LOGW(TAG, "screensaver direct task create failed; using LVGL timer");
        return false;
    }

    return true;
}

static void stop_direct_screensaver_task(void)
{
    if (s_screensaver_fx.direct_task == NULL) {
        return;
    }

    s_screensaver_fx.direct_stop_requested = true;
    if (s_screensaver_fx.direct_task_done == NULL) {
        return;
    }

    if (xSemaphoreTake(s_screensaver_fx.direct_task_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "screensaver direct task stop timeout");
        return;
    }

    while (xSemaphoreTake(s_screensaver_fx.direct_task_done, 0) == pdTRUE) {
    }
}

static void start_screensaver_fx(void)
{
    if (s_screensaver_fx.timer == NULL) {
        return;
    }

    s_screensaver_fx.frame_count = 0;
    s_screensaver_fx.fps_x10 = 0;
    s_screensaver_fx.interval_ms_x10 = 0;
    s_screensaver_fx.render_ms_x10 = 0;
    s_screensaver_fx.perf_frames = 0;
    s_screensaver_fx.perf_interval_samples = 0;
    s_screensaver_fx.perf_render_total_us = 0;
    s_screensaver_fx.perf_interval_total_us = 0;
    s_screensaver_fx.perf_window_start_us = 0;
    s_screensaver_fx.prev_frame_start_us = 0;
    s_screensaver_fx.time_origin_us = 0;
    s_screensaver_fx.time_ms = 0;
    s_screensaver_fx.last_time_refresh_us = 0;
    s_screensaver_fx.direct_push_warned = false;
    s_screensaver_fx.direct_stop_requested = false;
    s_screensaver_fx.direct_active =
        screensaver_direct_is_ready() && bsp_display_begin_direct_mode();
    if (screensaver_direct_is_ready() && !s_screensaver_fx.direct_active) {
        ESP_LOGW(TAG, "screensaver direct mode unavailable; falling back to LVGL path");
    }
    if (s_screensaver_fx.direct_active) {
        screensaver_direct_reset();
    }
    set_screensaver_overlay_children_hidden(s_screensaver_fx.direct_active);
    update_screensaver_perf_label();
    if (s_screensaver_fx.direct_active) {
        if (!start_direct_screensaver_task()) {
            bsp_display_end_direct_mode();
            screensaver_direct_restore_background();
            s_screensaver_fx.direct_active = false;
            set_screensaver_overlay_children_hidden(false);
            render_screensaver_background();
            lv_timer_resume(s_screensaver_fx.timer);
        }
    } else {
        render_screensaver_background();
        lv_timer_resume(s_screensaver_fx.timer);
    }
}

static void stop_screensaver_fx(void)
{
    if (s_screensaver_fx.timer != NULL) {
        lv_timer_pause(s_screensaver_fx.timer);
    }

    if (s_screensaver_fx.direct_active) {
        stop_direct_screensaver_task();
        if (!screensaver_direct_wait_idle(1000)) {
            ESP_LOGW(TAG, "screensaver direct pipeline idle wait timeout");
        }
        bsp_display_end_direct_mode();
        screensaver_direct_restore_background();
        s_screensaver_fx.direct_active = false;
        s_screensaver_fx.direct_stop_requested = false;
        set_screensaver_overlay_children_hidden(false);
        lv_obj_invalidate(lv_screen_active());
    }
}

static void update_screensaver_time_label(const home_snapshot_t *snapshot)
{
    char time_text[6] = "--:--";

    if (snapshot != NULL && snapshot->time_text[0] != '\0' && snapshot->time_text[0] != '-') {
        memcpy(time_text, snapshot->time_text, 5);
        time_text[5] = '\0';
    }

    memcpy(s_screensaver_fx.time_text, time_text, sizeof(time_text));

    if (!s_screensaver_fx.direct_active && s_view.screensaver_time_label != NULL) {
        lv_label_set_text(s_view.screensaver_time_label, time_text);
    }
}

static void set_normal_view_hidden(bool hidden)
{
    if (s_view.status_bar != NULL) {
        if (hidden) {
            lv_obj_add_flag(s_view.status_bar, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_view.status_bar, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_view.time_label != NULL) {
        if (hidden) {
            lv_obj_add_flag(s_view.time_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_view.time_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_view.date_label != NULL) {
        if (hidden) {
            lv_obj_add_flag(s_view.date_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_view.date_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_view.weather_row != NULL) {
        if (hidden) {
            lv_obj_add_flag(s_view.weather_row, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_view.weather_row, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_view.sprite_img != NULL) {
        if (hidden) {
            lv_obj_add_flag(s_view.sprite_img, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_view.sprite_img, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (hidden && s_view.bubble_box != NULL) {
        lv_obj_add_flag(s_view.bubble_box, LV_OBJ_FLAG_HIDDEN);
    }
}

static void enter_screensaver(const home_snapshot_t *snapshot)
{
    if (s_display_mode == HOME_DISPLAY_SCREENSAVER) {
        update_screensaver_time_label(snapshot);
        start_screensaver_fx();
        return;
    }

    s_display_mode = HOME_DISPLAY_SCREENSAVER;
    if (s_sprite.timer != NULL) {
        lv_timer_pause(s_sprite.timer);
    }
    if (s_time_refresh_timer != NULL) {
        lv_timer_pause(s_time_refresh_timer);
    }
    if (s_bubble_timer != NULL) {
        lv_timer_delete(s_bubble_timer);
        s_bubble_timer = NULL;
    }
    set_normal_view_hidden(true);
    update_screensaver_time_label(snapshot);
    if (s_view.screensaver_overlay != NULL) {
        lv_obj_clear_flag(s_view.screensaver_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    start_screensaver_fx();
}

static void exit_screensaver(void)
{
    if (s_display_mode != HOME_DISPLAY_SCREENSAVER) {
        return;
    }

    s_display_mode = HOME_DISPLAY_NORMAL;
    stop_screensaver_fx();
    if (s_view.screensaver_overlay != NULL) {
        lv_obj_add_flag(s_view.screensaver_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    set_normal_view_hidden(false);
    if (s_sprite.timer != NULL) {
        lv_timer_resume(s_sprite.timer);
    }
    if (s_time_refresh_timer != NULL) {
        lv_timer_resume(s_time_refresh_timer);
    }
}

static void screensaver_touch_cb(lv_event_t *e)
{
    (void)e;
    if (s_display_mode == HOME_DISPLAY_SCREENSAVER) {
        reset_screensaver_idle_deadline();
        exit_screensaver();
        home_service_refresh_snapshot();
        refresh_view();
    }
}

static void create_screensaver_overlay(lv_obj_t *root)
{
    lv_obj_t *overlay = lv_obj_create(root);
    size_t fx_buf_size = HOME_SCREENSAVER_FX_W * HOME_SCREENSAVER_FX_H * sizeof(lv_color32_t);

    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(HOME_BG_BASE_COLOR), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(overlay, LV_ALIGN_TOP_LEFT, -16, -16);
    lv_obj_add_event_cb(overlay, screensaver_touch_cb, LV_EVENT_PRESSED, NULL);
    s_view.screensaver_overlay = overlay;

    s_screensaver_fx.buf = heap_caps_malloc(fx_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_screensaver_fx.buf == NULL) {
        s_screensaver_fx.buf = heap_caps_malloc(fx_buf_size, MALLOC_CAP_8BIT);
    }

    if (s_screensaver_fx.buf != NULL) {
        s_screensaver_fx.canvas = lv_canvas_create(overlay);
        lv_canvas_set_buffer(s_screensaver_fx.canvas, s_screensaver_fx.buf, HOME_SCREENSAVER_FX_W,
                             HOME_SCREENSAVER_FX_H, LV_COLOR_FORMAT_ARGB8888);
        lv_obj_add_flag(s_screensaver_fx.canvas, LV_OBJ_FLAG_HIDDEN);

        s_screensaver_fx.image = lv_image_create(overlay);
        lv_image_set_src(s_screensaver_fx.image, lv_canvas_get_image(s_screensaver_fx.canvas));
        lv_image_set_scale(s_screensaver_fx.image, HOME_SCREENSAVER_FX_SCALE);
        lv_obj_center(s_screensaver_fx.image);

        balatro_init(HOME_SCREENSAVER_FX_W, HOME_SCREENSAVER_FX_H);
        if (!screensaver_direct_init()) {
            ESP_LOGW(TAG, "screensaver direct buffer alloc failed; using LVGL fallback");
        }

        s_screensaver_fx.time_ms = 0;
        s_screensaver_fx.time_origin_us = 0;
        memcpy(s_screensaver_fx.time_text, "--:--", sizeof(s_screensaver_fx.time_text));
        render_screensaver_background();

        s_screensaver_fx.timer =
            lv_timer_create(screensaver_fx_timer_cb, HOME_SCREENSAVER_FX_PERIOD_MS, NULL);
        lv_timer_pause(s_screensaver_fx.timer);
    } else {
        ESP_LOGW(TAG, "screensaver fx buffer allocation failed; using static background");
    }

    s_view.screensaver_time_label = lv_label_create(overlay);
    lv_obj_set_style_text_font(s_view.screensaver_time_label, &departure_mono_55, 0);
    lv_obj_set_style_text_color(s_view.screensaver_time_label,
                                lv_color_hex(HOME_SCREENSAVER_TIME_COLOR), 0);
    lv_obj_set_style_text_letter_space(s_view.screensaver_time_label,
                                       HOME_SCREENSAVER_TIME_LETTER_SPACE, 0);
    lv_obj_center(s_view.screensaver_time_label);
    lv_label_set_text_static(s_view.screensaver_time_label, "--:--");
}

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

static void unread_auto_clear_cb(lv_timer_t *t)
{
    (void)t;
    claude_service_mark_read(s_unread_seq);
    s_unread_timer = NULL;
}

static void schedule_unread_clear(uint32_t seq)
{
    s_unread_seq = seq;
    if (s_unread_timer != NULL) {
        lv_timer_reset(s_unread_timer);
        return;
    }
    s_unread_timer = lv_timer_create(unread_auto_clear_cb, HOME_UNREAD_AUTO_CLEAR_MS, NULL);
    lv_timer_set_repeat_count(s_unread_timer, 1);
}

static void refresh_status_bar(const home_snapshot_t *snapshot)
{
    lv_color_t wifi_color = lv_color_hex(HOME_ICON_MUTED_COLOR);
    lv_color_t claude_color = lv_color_hex(HOME_ICON_MUTED_COLOR);

    if (snapshot->wifi_connected) {
        wifi_color = lv_color_hex(HOME_WIFI_ONLINE_COLOR);
        lv_label_set_text_static(s_view.wifi_icon, APP_HOME_SYMBOL_WIFI);
    } else if (snapshot->wifi_connecting) {
        wifi_color = lv_color_hex(HOME_WIFI_CONNECTING_COLOR);
        lv_label_set_text_static(s_view.wifi_icon, APP_HOME_SYMBOL_WIFI_1);
    } else {
        lv_label_set_text_static(s_view.wifi_icon, APP_HOME_SYMBOL_WIFI_OFF);
    }

    if (snapshot->claude_unread) {
        claude_color = lv_color_hex(HOME_CLAUDE_ACTIVE_COLOR);
    } else if (snapshot->claude_connected) {
        claude_color = lv_color_hex(HOME_CLAUDE_ACTIVE_COLOR);
    }

    lv_obj_set_style_text_color(s_view.wifi_icon, wifi_color, 0);
    lv_obj_set_style_text_color(s_view.claude_icon, claude_color, 0);
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

static void refresh_sprite(const home_snapshot_t *snapshot)
{
    sprite_state_t new_state;

    if (s_view.sprite_img == NULL) {
        return;
    }

    new_state = map_run_state(snapshot->claude_run_state, snapshot->claude_connected);
    if (new_state != s_sprite.state || s_sprite.anim == NULL) {
        s_sprite.state = new_state;
        s_sprite.anim = &s_sprite_anims[new_state];
        s_sprite.frame_idx = 0;
        lv_image_set_src(s_view.sprite_img, &s_sprite.anim->frames[0]);
        if (s_sprite.timer != NULL) {
            lv_timer_set_period(s_sprite.timer, s_sprite.anim->period_ms);
        }
    }
}

static void bubble_fade_cb(lv_timer_t *t)
{
    (void)t;
    s_bubble_dismissed = true;
    if (s_view.bubble_box != NULL) {
        lv_obj_add_flag(s_view.bubble_box, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_bubble_timer != NULL) {
        lv_timer_delete(s_bubble_timer);
        s_bubble_timer = NULL;
    }
}

static void refresh_bubble(const home_snapshot_t *snapshot)
{
    bool show;

    if (s_view.bubble_box == NULL) {
        return;
    }

    show = (snapshot->claude_detail[0] != '\0') && (s_sprite.state != SPRITE_STATE_SLEEPING);

    if (show) {
        const char *cur = lv_label_get_text(s_view.bubble_label);
        bool text_changed = (cur == NULL || strcmp(cur, snapshot->claude_detail) != 0);

        if (text_changed) {
            s_bubble_dismissed = false;
            lv_label_set_text(s_view.bubble_label, snapshot->claude_detail);
            lv_obj_clear_flag(s_view.bubble_box, LV_OBJ_FLAG_HIDDEN);
            if (s_bubble_timer != NULL) {
                lv_timer_delete(s_bubble_timer);
            }
            s_bubble_timer = lv_timer_create(bubble_fade_cb, HOME_BUBBLE_FADE_MS, NULL);
            lv_timer_set_repeat_count(s_bubble_timer, 1);
        } else if (!s_bubble_dismissed) {
            lv_obj_clear_flag(s_view.bubble_box, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        lv_obj_add_flag(s_view.bubble_box, LV_OBJ_FLAG_HIDDEN);
        s_bubble_dismissed = false;
        if (s_bubble_timer != NULL) {
            lv_timer_delete(s_bubble_timer);
            s_bubble_timer = NULL;
        }
    }
}

static void refresh_view(void)
{
    home_snapshot_t snapshot;
    char date_line[48];

    if (s_view.root == NULL) {
        return;
    }

    home_service_get_snapshot(&snapshot);

    if (s_display_mode == HOME_DISPLAY_SCREENSAVER) {
        update_screensaver_time_label(&snapshot);
        return;
    }

    snprintf(date_line, sizeof(date_line), "%s  %s", snapshot.date_text, snapshot.weekday_text);

    lv_label_set_text(s_view.time_label, snapshot.time_text);
    lv_label_set_text(s_view.date_label, date_line);
    refresh_weather_summary(&snapshot);
    refresh_status_bar(&snapshot);
    refresh_sprite(&snapshot);
    refresh_bubble(&snapshot);
}

static esp_err_t app_home_init(void)
{
    home_service_refresh_snapshot();
    return ESP_OK;
}

/* ---- Approval overlay ---- */

static void approval_btn_cb(lv_event_t *e)
{
    approval_decision_t decision = (approval_decision_t)(intptr_t)lv_event_get_user_data(e);
    device_link_resolve_approval(decision);
    lv_obj_add_flag(s_view.approval_overlay, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *create_approval_btn(lv_obj_t *parent, const char *text, uint32_t color,
                                     approval_decision_t decision)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_height(btn, APPROVE_BTN_H);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, ui_font_text_11(), 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_center(label);

    lv_obj_add_event_cb(btn, approval_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)decision);
    return btn;
}

static void create_approval_overlay(lv_obj_t *root)
{
    lv_obj_t *overlay = lv_obj_create(root);
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(APPROVE_BG_COLOR), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_90, 0);
    lv_obj_set_style_pad_hor(overlay, 16, 0);
    lv_obj_set_style_pad_ver(overlay, 10, 0);
    /* Float so it doesn't participate in root's layout */
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_FLOATING);
    /* Cancel root's padding so overlay covers full screen */
    lv_obj_align(overlay, LV_ALIGN_TOP_LEFT, -16, -16);
    s_view.approval_overlay = overlay;

    /* Info row (top): tool name + description side by side */
    s_view.approval_tool_label = lv_label_create(overlay);
    lv_obj_set_style_text_font(s_view.approval_tool_label, ui_font_text_11(), 0);
    lv_obj_set_style_text_color(s_view.approval_tool_label, lv_color_hex(0xe9e0cf), 0);
    lv_label_set_long_mode(s_view.approval_tool_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_view.approval_tool_label, 132);
    lv_obj_align(s_view.approval_tool_label, LV_ALIGN_TOP_LEFT, 0, 8);

    s_view.approval_desc_label = lv_label_create(overlay);
    lv_obj_set_style_text_font(s_view.approval_desc_label, ui_font_text_11(), 0);
    lv_obj_set_style_text_color(s_view.approval_desc_label, lv_color_hex(0x8899a6), 0);
    lv_obj_set_width(s_view.approval_desc_label, BSP_LCD_H_RES - 32 - 140);
    lv_label_set_long_mode(s_view.approval_desc_label, LV_LABEL_LONG_DOT);
    lv_obj_align(s_view.approval_desc_label, LV_ALIGN_TOP_LEFT, 140, 10);

    /* Button row (bottom): three buttons side by side */
    lv_obj_t *btn_row = lv_obj_create(overlay);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_size(btn_row, BSP_LCD_H_RES - 32, BSP_LCD_V_RES - APPROVE_INFO_H - 20);
    lv_obj_align(btn_row, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_row, APPROVE_BTN_GAP, 0);

    s_view.approval_btn_allow =
        create_approval_btn(btn_row, "Accept", APPROVE_ALLOW_COLOR, APPROVAL_DECISION_ALLOW);
    s_view.approval_btn_deny =
        create_approval_btn(btn_row, "Decline", APPROVE_DENY_COLOR, APPROVAL_DECISION_DENY);
    s_view.approval_btn_yolo =
        create_approval_btn(btn_row, "YOLO", APPROVE_YOLO_COLOR, APPROVAL_DECISION_YOLO);
}

static void show_approval_overlay(void)
{
    approval_request_t req;
    if (!device_link_get_pending_approval(&req)) {
        return;
    }
    lv_label_set_text(s_view.approval_tool_label, req.tool_name);
    lv_label_set_text(s_view.approval_desc_label, req.description);
    lv_obj_clear_flag(s_view.approval_overlay, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *app_home_create_root(lv_obj_t *parent)
{
    lv_obj_t *root = lv_obj_create(parent);

    s_view.root = root;
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(root, lv_color_hex(HOME_BG_BASE_COLOR), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(root, 16, 0);

    create_screensaver_overlay(root);

    /* ---- left half: status / clock / date / weather ---- */

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
    lv_obj_set_style_text_font(s_view.time_label, ui_font_display_44(), 0);
    lv_obj_set_style_text_color(s_view.time_label, lv_color_hex(0xa6f0ff), 0);
    lv_obj_align(s_view.time_label, LV_ALIGN_TOP_LEFT, 0, HOME_TIME_Y);

    s_view.date_label = lv_label_create(root);
    lv_obj_set_style_text_font(s_view.date_label, ui_font_text_22(), 0);
    lv_obj_set_style_text_color(s_view.date_label, lv_color_hex(0xb7c4cc), 0);
    lv_obj_align(s_view.date_label, LV_ALIGN_TOP_LEFT, 0, HOME_DATE_Y);

    s_view.weather_row = lv_obj_create(root);
    lv_obj_remove_style_all(s_view.weather_row);
    lv_obj_set_size(s_view.weather_row, HOME_LEFT_HALF_W, HOME_WEATHER_ROW_HEIGHT);
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
                     HOME_LEFT_HALF_W - HOME_STATUS_ITEM_BOX_SIZE - HOME_WEATHER_ICON_GAP);
    lv_label_set_long_mode(s_view.weather_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(s_view.weather_label, ui_font_text_11(), 0);
    lv_obj_set_style_text_color(s_view.weather_label, lv_color_hex(HOME_WEATHER_TEXT_COLOR), 0);
    lv_obj_set_style_translate_y(s_view.weather_label, HOME_WEATHER_TEXT_Y_OFFSET, 0);

    /* ---- right half: sprite ---- */

    s_view.sprite_img = lv_image_create(root);
    lv_image_set_src(s_view.sprite_img, &sprite_idle_frames[0]);
    lv_image_set_scale(s_view.sprite_img, HOME_SPRITE_SCALE);
    lv_obj_set_style_image_recolor(s_view.sprite_img, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(s_view.sprite_img, LV_OPA_TRANSP, 0);
    lv_obj_align(s_view.sprite_img, LV_ALIGN_RIGHT_MID, -60, 0);

    /* ---- bubble above sprite ---- */

    s_view.bubble_box = lv_obj_create(root);
    lv_obj_remove_style_all(s_view.bubble_box);
    lv_obj_set_size(s_view.bubble_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(s_view.bubble_box, HOME_BUBBLE_MAX_W, 0);
    lv_obj_set_style_bg_color(s_view.bubble_box, lv_color_hex(HOME_BUBBLE_BG_COLOR), 0);
    lv_obj_set_style_bg_opa(s_view.bubble_box, LV_OPA_80, 0);
    lv_obj_set_style_radius(s_view.bubble_box, HOME_BUBBLE_RADIUS, 0);
    lv_obj_set_style_pad_hor(s_view.bubble_box, HOME_BUBBLE_PAD_H, 0);
    lv_obj_set_style_pad_ver(s_view.bubble_box, HOME_BUBBLE_PAD_V, 0);
    lv_obj_align(s_view.bubble_box, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_flag(s_view.bubble_box, LV_OBJ_FLAG_HIDDEN);

    s_view.bubble_label = lv_label_create(s_view.bubble_box);
    lv_obj_set_style_max_width(s_view.bubble_label, HOME_BUBBLE_MAX_W - 2 * HOME_BUBBLE_PAD_H, 0);
    lv_label_set_long_mode(s_view.bubble_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_view.bubble_label, ui_font_text_11(), 0);
    lv_obj_set_style_text_color(s_view.bubble_label, lv_color_hex(HOME_BUBBLE_TEXT_COLOR), 0);
    lv_label_set_text_static(s_view.bubble_label, "");

    s_sprite.state = SPRITE_STATE_IDLE;
    s_sprite.anim = &s_sprite_anims[SPRITE_STATE_IDLE];
    s_sprite.frame_idx = 0;
    s_sprite.timer = lv_timer_create(sprite_timer_cb, s_sprite.anim->period_ms, NULL);

    s_time_refresh_timer = lv_timer_create(time_refresh_cb, HOME_TIME_REFRESH_MS, NULL);

    /* ---- approval overlay (must be last for z-order) ---- */
    create_approval_overlay(root);

    s_display_mode = HOME_DISPLAY_NORMAL;
    reset_screensaver_idle_deadline();
    refresh_view();
    return root;
}

static void app_home_resume(void)
{
    home_service_refresh_snapshot();
    s_display_mode = HOME_DISPLAY_NORMAL;
    reset_screensaver_idle_deadline();
    if (s_view.screensaver_overlay != NULL) {
        lv_obj_add_flag(s_view.screensaver_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    stop_screensaver_fx();
    set_normal_view_hidden(false);
    refresh_view();
    if (s_sprite.timer != NULL) {
        lv_timer_resume(s_sprite.timer);
    }
    if (s_time_refresh_timer != NULL) {
        lv_timer_resume(s_time_refresh_timer);
    }
}

static void app_home_suspend(void)
{
    if (s_sprite.timer != NULL) {
        lv_timer_pause(s_sprite.timer);
    }
    if (s_time_refresh_timer != NULL) {
        lv_timer_pause(s_time_refresh_timer);
    }
    if (s_bubble_timer != NULL) {
        lv_timer_delete(s_bubble_timer);
        s_bubble_timer = NULL;
    }
    if (s_unread_timer != NULL) {
        lv_timer_delete(s_unread_timer);
        s_unread_timer = NULL;
    }
    stop_screensaver_fx();
}

static esp_err_t app_home_handle_control(app_control_type_t type, const void *payload)
{
    if (s_view.root == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    switch (type) {
    case APP_CONTROL_HOME_SCREENSAVER: {
        const app_control_home_screensaver_t *control = payload;
        home_snapshot_t snapshot;

        ESP_RETURN_ON_FALSE(control != NULL, ESP_ERR_INVALID_ARG, "app_home",
                            "screensaver control is required");
        home_service_refresh_snapshot();
        home_service_get_snapshot(&snapshot);

        if (control->enabled) {
            enter_screensaver(&snapshot);
        } else {
            reset_screensaver_idle_deadline();
            exit_screensaver();
            refresh_view();
        }
        return ESP_OK;
    }
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

static void app_home_handle_event(const app_event_t *event)
{
    if (event == NULL) {
        return;
    }

    switch (event->type) {
    case APP_EVENT_TICK_1S:
        home_service_refresh_snapshot();
        if (s_display_mode != HOME_DISPLAY_SCREENSAVER &&
            (home_now_us() - s_last_claude_activity_us) >= HOME_SCREENSAVER_IDLE_US) {
            home_snapshot_t snap;
            home_service_get_snapshot(&snap);
            enter_screensaver(&snap);
            break;
        }
        refresh_view();
        break;
    case APP_EVENT_POWER_CHANGED:
    case APP_EVENT_NET_CHANGED:
    case APP_EVENT_DATA_WEATHER:
        home_service_refresh_snapshot();
        refresh_view();
        break;
    case APP_EVENT_DATA_CLAUDE: {
        home_service_refresh_snapshot();
        home_snapshot_t snap;
        home_service_get_snapshot(&snap);
        reset_screensaver_idle_deadline();
        exit_screensaver();

        /* T3: auto-dismiss approval overlay on disconnect */
        if (s_was_connected && !snap.claude_connected) {
            if (s_view.approval_overlay != NULL &&
                !lv_obj_has_flag(s_view.approval_overlay, LV_OBJ_FLAG_HIDDEN)) {
                device_link_cancel_approval();
                lv_obj_add_flag(s_view.approval_overlay, LV_OBJ_FLAG_HIDDEN);
            }
        }
        s_was_connected = snap.claude_connected;

        /* T6: auto-clear unread dot after 5s */
        if (snap.claude_unread) {
            claude_snapshot_t cs;
            claude_service_get_snapshot(&cs);
            schedule_unread_clear(cs.seq);
        }

        refresh_view();
        break;
    }
    case APP_EVENT_PERMISSION_REQUEST:
        reset_screensaver_idle_deadline();
        exit_screensaver();
        show_approval_overlay();
        break;
    case APP_EVENT_PERMISSION_DISMISS:
        if (s_view.approval_overlay != NULL) {
            lv_obj_add_flag(s_view.approval_overlay, LV_OBJ_FLAG_HIDDEN);
        }
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
        .suspend = app_home_suspend,
        .handle_event = app_home_handle_event,
        .handle_control = app_home_handle_control,
    };

    return &descriptor;
}
