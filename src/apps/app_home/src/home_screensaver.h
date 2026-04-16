#ifndef APP_HOME_HOME_SCREENSAVER_H
#define APP_HOME_HOME_SCREENSAVER_H

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "home_presenter.h"
#include "lvgl.h"

typedef void (*home_screensaver_touch_cb_t)(void *ctx);

typedef struct {
    lv_obj_t *canvas;
    lv_obj_t *image;
    lv_timer_t *timer;
    uint8_t *buf;
    bool direct_active;
    uint32_t time_ms;
    uint16_t fps_x10;
    uint16_t interval_ms_x10;
    uint16_t render_ms_x10;
    uint16_t perf_frames;
    uint16_t perf_interval_samples;
    uint32_t perf_render_total_us;
    uint64_t perf_interval_total_us;
    int64_t perf_window_start_us;
    int64_t perf_log_muted_until_us;
    int64_t prev_frame_start_us;
    int64_t time_origin_us;
    int64_t last_time_refresh_us;
    bool direct_push_warned;
    volatile bool direct_stop_requested;
    TaskHandle_t direct_task;
    SemaphoreHandle_t direct_task_done;
    char time_text[6];
} home_screensaver_fx_t;

typedef struct {
    lv_obj_t *overlay;
    lv_obj_t *time_label;
    bool active;
    int64_t last_activity_us;
    home_screensaver_touch_cb_t touch_cb;
    void *touch_cb_ctx;
    home_screensaver_fx_t fx;
} home_screensaver_t;

void home_screensaver_create(home_screensaver_t *screensaver, lv_obj_t *root,
                             home_screensaver_touch_cb_t touch_cb, void *touch_cb_ctx);
void home_screensaver_enter(home_screensaver_t *screensaver, const home_present_model_t *model);
void home_screensaver_exit(home_screensaver_t *screensaver);
void home_screensaver_suspend(home_screensaver_t *screensaver);
void home_screensaver_apply(home_screensaver_t *screensaver, const home_present_model_t *model);
void home_screensaver_poke_activity(home_screensaver_t *screensaver);
void home_screensaver_mute_perf_logs(home_screensaver_t *screensaver, uint32_t duration_ms);
bool home_screensaver_should_enter(const home_screensaver_t *screensaver, int64_t now_us);
bool home_screensaver_is_active(const home_screensaver_t *screensaver);

#endif
