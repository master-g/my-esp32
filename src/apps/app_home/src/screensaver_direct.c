#include "screensaver_direct.h"

#include <stdio.h>
#include <string.h>

#include "bsp_board_config.h"
#include "bsp_display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "screensaver_effects.h"
#include "screensaver_glyphs.h"

#define TAG "screensaver_direct"
#define DIRECT_LOGICAL_W BSP_LCD_H_RES
#define DIRECT_LOGICAL_H BSP_LCD_V_RES
#define DIRECT_TIME_SCALE 7
#define DIRECT_TIME_CHAR_W 5
#define DIRECT_TIME_CHAR_H 7
#define DIRECT_TIME_CHAR_COUNT 5
#define DIRECT_FRAMEBUF_COUNT 2
#define DIRECT_PERF_WINDOW_US 500000
#define DIRECT_PUSH_TASK_STACK_SIZE (4 * 1024)
#define DIRECT_PUSH_TASK_PRIORITY 2
#define DIRECT_PUSH_TASK_CORE 0
#define DIRECT_PUSH_STOP_TOKEN 0xFFU

typedef struct {
    int64_t frame_start_us;
    uint32_t compose_us;
    uint32_t text_us;
} direct_frame_perf_t;

static uint16_t *s_framebufs[DIRECT_FRAMEBUF_COUNT];
static uint16_t *s_framebuf;
static QueueHandle_t s_free_queue;
static QueueHandle_t s_ready_queue;
static TaskHandle_t s_push_task;
static SemaphoreHandle_t s_push_task_done;
static SemaphoreHandle_t s_render_mutex;
static direct_frame_perf_t s_frame_perf[DIRECT_FRAMEBUF_COUNT];
static bool s_ready;
static volatile bool s_push_stop_requested;
static struct {
    uint16_t compose_ms_x10;
    uint16_t text_ms_x10;
    uint16_t wait_ms_x10;
    uint16_t push_ms_x10;
} s_perf_snapshot;
static uint16_t s_perf_frames;
static uint64_t s_perf_compose_total_us;
static uint64_t s_perf_text_total_us;
static uint64_t s_perf_wait_total_us;
static uint64_t s_perf_push_total_us;
static int64_t s_perf_window_start_us;

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8U) << 8) | ((g & 0xFCU) << 3) | (b >> 3));
}

static inline uint16_t avg_us_to_ms_x10(uint64_t total_us, uint16_t frames)
{
    if (frames == 0) {
        return 0;
    }

    return (uint16_t)((total_us * 10ULL + ((uint64_t)frames * 500ULL)) /
                      ((uint64_t)frames * 1000ULL));
}

static inline uint32_t native_index_from_logical(int32_t x, int32_t y)
{
#if BSP_UI_ROTATION == BSP_UI_ROTATION_LANDSCAPE_270
    uint32_t native_x = (uint32_t)(BSP_LCD_PANEL_H_RES - 1 - y);
    uint32_t native_y = (uint32_t)x;
#else
    uint32_t native_x = (uint32_t)y;
    uint32_t native_y = (uint32_t)(BSP_LCD_PANEL_V_RES - 1 - x);
#endif
    return native_y * BSP_LCD_PANEL_H_RES + native_x;
}

static void put_pixel_native(void *ctx, int32_t x, int32_t y, uint16_t color)
{
    (void)ctx;
    if (x < 0 || y < 0 || x >= DIRECT_LOGICAL_W || y >= DIRECT_LOGICAL_H || s_framebuf == NULL) {
        return;
    }

    s_framebuf[native_index_from_logical(x, y)] = color;
}

static void put_pixel_logical_buffer(void *ctx, int32_t x, int32_t y, uint16_t color)
{
    uint16_t *pixels = ctx;

    if (pixels == NULL || x < 0 || y < 0 || x >= DIRECT_LOGICAL_W || y >= DIRECT_LOGICAL_H) {
        return;
    }

    pixels[(size_t)y * DIRECT_LOGICAL_W + x] = color;
}

#define SS_CLOCK_MARGIN 10

/* Halve each RGB565 channel twice -> ~25% brightness scrim. */
static inline uint16_t rgb565_darken(uint16_t c)
{
    c = (uint16_t)((c >> 1) & 0x7BEFU);
    return (uint16_t)((c >> 1) & 0x7BEFU);
}

/* Darken the native framebuffer under the clock so it stays legible over any
 * effect. Rectangle is in logical coords; mapped per pixel to native. */
static void scrim_clock_region(int32_t x0, int32_t y0, int32_t w, int32_t h)
{
    for (int32_t y = y0; y < y0 + h; y++) {
        for (int32_t x = x0; x < x0 + w; x++) {
            if (x < 0 || y < 0 || x >= DIRECT_LOGICAL_W || y >= DIRECT_LOGICAL_H ||
                s_framebuf == NULL) {
                continue;
            }
            uint32_t idx = native_index_from_logical(x, y);
            s_framebuf[idx] = rgb565_darken(s_framebuf[idx]);
        }
    }
}

/* Clear the native framebuffer and render the selected effect's background. */
static void effects_render_native(uint32_t time_ms)
{
    memset(s_framebuf, 0, (size_t)BSP_LCD_PANEL_H_RES * BSP_LCD_PANEL_V_RES * sizeof(uint16_t));
    screensaver_effects_render(put_pixel_native, NULL, DIRECT_LOGICAL_W / SS_CELL_W,
                               DIRECT_LOGICAL_H / SS_CELL_H, time_ms);
}

static void note_direct_perf(int64_t frame_start_us, int64_t frame_end_us, uint32_t compose_us,
                             uint32_t text_us, uint32_t wait_us, uint32_t push_us)
{
    if (s_perf_window_start_us == 0) {
        s_perf_window_start_us = frame_start_us;
    }

    s_perf_frames++;
    s_perf_compose_total_us += compose_us;
    s_perf_text_total_us += text_us;
    s_perf_wait_total_us += wait_us;
    s_perf_push_total_us += push_us;

    if ((frame_end_us - s_perf_window_start_us) >= DIRECT_PERF_WINDOW_US) {
        s_perf_snapshot.compose_ms_x10 = avg_us_to_ms_x10(s_perf_compose_total_us, s_perf_frames);
        s_perf_snapshot.text_ms_x10 = avg_us_to_ms_x10(s_perf_text_total_us, s_perf_frames);
        s_perf_snapshot.wait_ms_x10 = avg_us_to_ms_x10(s_perf_wait_total_us, s_perf_frames);
        s_perf_snapshot.push_ms_x10 = avg_us_to_ms_x10(s_perf_push_total_us, s_perf_frames);

        s_perf_frames = 0;
        s_perf_compose_total_us = 0;
        s_perf_text_total_us = 0;
        s_perf_wait_total_us = 0;
        s_perf_push_total_us = 0;
        s_perf_window_start_us = frame_end_us;
    }
}

static void screensaver_direct_push_task(void *arg)
{
    (void)arg;

    for (;;) {
        bsp_display_push_stats_t push_stats = {0};
        uint8_t frame_idx = 0;
        esp_err_t err;
        int64_t frame_end_us;

        if (xQueueReceive(s_ready_queue, &frame_idx, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (s_push_stop_requested && frame_idx == DIRECT_PUSH_STOP_TOKEN) {
            break;
        }
        if (frame_idx >= DIRECT_FRAMEBUF_COUNT) {
            continue;
        }

        err = bsp_display_push_native_rgb565(s_framebufs[frame_idx], BSP_LCD_PANEL_V_RES, 0,
                                             &push_stats);
        frame_end_us = esp_timer_get_time();
        if (err == ESP_OK) {
            note_direct_perf(s_frame_perf[frame_idx].frame_start_us, frame_end_us,
                             s_frame_perf[frame_idx].compose_us, s_frame_perf[frame_idx].text_us,
                             push_stats.wait_us, push_stats.push_us);
        } else {
            ESP_LOGW(TAG, "direct push failed: %s", esp_err_to_name(err));
        }

        if (xQueueSend(s_free_queue, &frame_idx, portMAX_DELAY) != pdTRUE) {
            ESP_LOGW(TAG, "free buffer queue send failed");
        }
    }

    s_push_task = NULL;
    if (s_push_task_done != NULL) {
        xSemaphoreGive(s_push_task_done);
    }
    vTaskDelete(NULL);
}

bool screensaver_direct_init(void)
{
    size_t frame_bytes;

    if (s_ready) {
        return true;
    }

    frame_bytes = (size_t)BSP_LCD_PANEL_H_RES * BSP_LCD_PANEL_V_RES * sizeof(uint16_t);
    for (size_t i = 0; i < DIRECT_FRAMEBUF_COUNT; i++) {
        s_framebufs[i] = heap_caps_malloc(frame_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_framebufs[i] == NULL) {
            s_framebufs[i] = heap_caps_malloc(frame_bytes, MALLOC_CAP_8BIT);
        }
        if (s_framebufs[i] == NULL) {
            screensaver_direct_deinit();
            return false;
        }

        memset(s_framebufs[i], 0, frame_bytes);
    }

    s_free_queue = xQueueCreate(DIRECT_FRAMEBUF_COUNT, sizeof(uint8_t));
    s_ready_queue = xQueueCreate(DIRECT_FRAMEBUF_COUNT, sizeof(uint8_t));
    s_push_task_done = xSemaphoreCreateBinary();
    if (s_free_queue == NULL || s_ready_queue == NULL || s_push_task_done == NULL) {
        screensaver_direct_deinit();
        return false;
    }
    s_render_mutex = xSemaphoreCreateMutex();
    if (s_render_mutex == NULL) {
        screensaver_direct_deinit();
        return false;
    }

    for (uint8_t i = 0; i < DIRECT_FRAMEBUF_COUNT; i++) {
        if (xQueueSend(s_free_queue, &i, 0) != pdTRUE) {
            screensaver_direct_deinit();
            return false;
        }
    }

    if (xTaskCreatePinnedToCore(screensaver_direct_push_task, "ss_direct_push",
                                DIRECT_PUSH_TASK_STACK_SIZE, NULL, DIRECT_PUSH_TASK_PRIORITY,
                                &s_push_task, DIRECT_PUSH_TASK_CORE) != pdPASS) {
        s_push_task = NULL;
        screensaver_direct_deinit();
        return false;
    }

    screensaver_direct_reset();
    s_ready = true;
    ESP_LOGI(TAG, "glyph selftest %s", ss_glyph_selftest() ? "pass" : "FAIL");
    return true;
}

bool screensaver_direct_is_ready(void)
{
    return s_ready && s_framebufs[0] != NULL && s_framebufs[1] != NULL && s_free_queue != NULL &&
           s_ready_queue != NULL && s_push_task != NULL;
}

void screensaver_direct_reset(void)
{
    memset(&s_perf_snapshot, 0, sizeof(s_perf_snapshot));
    s_perf_frames = 0;
    s_perf_compose_total_us = 0;
    s_perf_text_total_us = 0;
    s_perf_wait_total_us = 0;
    s_perf_push_total_us = 0;
    s_perf_window_start_us = 0;
}

bool screensaver_direct_wait_idle(uint32_t timeout_ms)
{
    int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000LL;

    if (!screensaver_direct_is_ready()) {
        return false;
    }

    while (uxQueueMessagesWaiting(s_free_queue) != DIRECT_FRAMEBUF_COUNT) {
        if (esp_timer_get_time() >= deadline_us) {
            return false;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }

    return true;
}

void screensaver_direct_get_perf_snapshot(screensaver_direct_perf_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }

    out->compose_ms_x10 = s_perf_snapshot.compose_ms_x10;
    out->text_ms_x10 = s_perf_snapshot.text_ms_x10;
    out->wait_ms_x10 = s_perf_snapshot.wait_ms_x10;
    out->push_ms_x10 = s_perf_snapshot.push_ms_x10;
}

void screensaver_direct_deinit(void)
{
    if (s_push_task != NULL) {
        uint8_t stop_token = DIRECT_PUSH_STOP_TOKEN;

        screensaver_direct_wait_idle(1000);
        s_push_stop_requested = true;
        while (xSemaphoreTake(s_push_task_done, 0) == pdTRUE) {
        }
        if (s_ready_queue != NULL) {
            xQueueSend(s_ready_queue, &stop_token, pdMS_TO_TICKS(100));
        }
        if (xSemaphoreTake(s_push_task_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGW(TAG, "push task stop timeout");
            vTaskDelete(s_push_task);
            s_push_task = NULL;
        }
    }
    s_push_stop_requested = false;
    if (s_ready_queue != NULL) {
        vQueueDelete(s_ready_queue);
        s_ready_queue = NULL;
    }
    if (s_free_queue != NULL) {
        vQueueDelete(s_free_queue);
        s_free_queue = NULL;
    }
    if (s_push_task_done != NULL) {
        vSemaphoreDelete(s_push_task_done);
        s_push_task_done = NULL;
    }
    if (s_render_mutex != NULL) {
        vSemaphoreDelete(s_render_mutex);
        s_render_mutex = NULL;
    }
    for (size_t i = 0; i < DIRECT_FRAMEBUF_COUNT; i++) {
        if (s_framebufs[i] != NULL) {
            heap_caps_free(s_framebufs[i]);
            s_framebufs[i] = NULL;
        }
    }
    s_framebuf = NULL;
    s_ready = false;
}

esp_err_t screensaver_direct_render_and_push(uint32_t time_ms, const char *time_text)
{
    uint8_t frame_idx = 0;
    int64_t frame_start_us;
    int64_t compose_start_us;
    int64_t text_start_us;
    uint32_t compose_us;
    uint32_t text_us;
    int32_t time_scale = DIRECT_TIME_SCALE;
    int32_t time_width =
        (DIRECT_TIME_CHAR_COUNT * DIRECT_TIME_CHAR_W + (DIRECT_TIME_CHAR_COUNT - 1)) * time_scale;
    int32_t time_height = DIRECT_TIME_CHAR_H * time_scale;
    int32_t time_y = (DIRECT_LOGICAL_H - time_height) / 2;
    uint16_t time_color = rgb565(243, 248, 255);

    if (!screensaver_direct_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueueReceive(s_free_queue, &frame_idx, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    s_framebuf = s_framebufs[frame_idx];
    frame_start_us = esp_timer_get_time();

    if (s_render_mutex == NULL || xSemaphoreTake(s_render_mutex, portMAX_DELAY) != pdTRUE) {
        if (xQueueSend(s_free_queue, &frame_idx, 0) != pdTRUE) {
            ESP_LOGW(TAG, "free buffer recovery failed");
        }
        return ESP_ERR_INVALID_STATE;
    }

    compose_start_us = frame_start_us;
    effects_render_native(time_ms);
    compose_us = (uint32_t)(esp_timer_get_time() - compose_start_us);

    text_start_us = esp_timer_get_time();
    scrim_clock_region((DIRECT_LOGICAL_W - time_width) / 2 - SS_CLOCK_MARGIN,
                       time_y - SS_CLOCK_MARGIN, time_width + 2 * SS_CLOCK_MARGIN,
                       time_height + 2 * SS_CLOCK_MARGIN);
    ss_glyph_draw_text(put_pixel_native, NULL, (DIRECT_LOGICAL_W - time_width) / 2, time_y,
                       time_scale, (time_text != NULL) ? time_text : "--:--", time_color);
    text_us = (uint32_t)(esp_timer_get_time() - text_start_us);
    xSemaphoreGive(s_render_mutex);

    s_frame_perf[frame_idx].frame_start_us = frame_start_us;
    s_frame_perf[frame_idx].compose_us = compose_us;
    s_frame_perf[frame_idx].text_us = text_us;
    if (xQueueSend(s_ready_queue, &frame_idx, portMAX_DELAY) != pdTRUE) {
        if (xQueueSend(s_free_queue, &frame_idx, 0) != pdTRUE) {
            ESP_LOGW(TAG, "free buffer recovery failed");
        }
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t screensaver_direct_render_snapshot_rgb565(uint32_t time_ms, const char *time_text,
                                                    uint16_t *pixels, size_t capacity_bytes,
                                                    uint16_t *out_width, uint16_t *out_height,
                                                    uint16_t *out_stride_bytes)
{
    const size_t required_bytes = (size_t)DIRECT_LOGICAL_W * DIRECT_LOGICAL_H * sizeof(uint16_t);
    int32_t time_scale = DIRECT_TIME_SCALE;
    int32_t time_width =
        (DIRECT_TIME_CHAR_COUNT * DIRECT_TIME_CHAR_W + (DIRECT_TIME_CHAR_COUNT - 1)) * time_scale;
    int32_t time_height = DIRECT_TIME_CHAR_H * time_scale;
    int32_t time_y = (DIRECT_LOGICAL_H - time_height) / 2;
    uint16_t time_color = rgb565(243, 248, 255);

    if (!screensaver_direct_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pixels == NULL || capacity_bytes < required_bytes) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_render_mutex == NULL || xSemaphoreTake(s_render_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(pixels, 0, required_bytes);
    screensaver_effects_render(put_pixel_logical_buffer, pixels, DIRECT_LOGICAL_W / SS_CELL_W,
                               DIRECT_LOGICAL_H / SS_CELL_H, time_ms);
    {
        int32_t cx = (DIRECT_LOGICAL_W - time_width) / 2 - SS_CLOCK_MARGIN;
        int32_t cy = time_y - SS_CLOCK_MARGIN;
        int32_t cw = time_width + 2 * SS_CLOCK_MARGIN;
        int32_t ch = time_height + 2 * SS_CLOCK_MARGIN;
        for (int32_t y = cy; y < cy + ch; y++) {
            for (int32_t x = cx; x < cx + cw; x++) {
                if (x < 0 || y < 0 || x >= DIRECT_LOGICAL_W || y >= DIRECT_LOGICAL_H) {
                    continue;
                }
                pixels[(size_t)y * DIRECT_LOGICAL_W + x] =
                    rgb565_darken(pixels[(size_t)y * DIRECT_LOGICAL_W + x]);
            }
        }
    }
    ss_glyph_draw_text(put_pixel_logical_buffer, pixels, (DIRECT_LOGICAL_W - time_width) / 2,
                       time_y, time_scale, (time_text != NULL) ? time_text : "--:--", time_color);
    xSemaphoreGive(s_render_mutex);

    if (out_width != NULL) {
        *out_width = DIRECT_LOGICAL_W;
    }
    if (out_height != NULL) {
        *out_height = DIRECT_LOGICAL_H;
    }
    if (out_stride_bytes != NULL) {
        *out_stride_bytes = DIRECT_LOGICAL_W * sizeof(uint16_t);
    }

    return ESP_OK;
}
