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
#include "screensaver_balatro.h"

#define TAG "screensaver_direct"
#define DIRECT_LOGICAL_W BSP_LCD_H_RES
#define DIRECT_LOGICAL_H BSP_LCD_V_RES
#define DIRECT_BG_W 136
#define DIRECT_BG_H 36
#define DIRECT_BG_VIEW_W_PCT 84U
#define DIRECT_BG_VIEW_H_PCT 90U
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
    char ch;
    uint8_t rows[7];
} glyph_t;

static const glyph_t s_glyphs[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},
    {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}},
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x14, 0x04, 0x04, 0x04, 0x1F}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x1C}},
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}},
    {'X', {0x11, 0x0A, 0x04, 0x04, 0x04, 0x0A, 0x11}},
};

typedef struct {
    int64_t frame_start_us;
    uint32_t compose_us;
    uint32_t text_us;
} direct_frame_perf_t;

static uint16_t *s_framebufs[DIRECT_FRAMEBUF_COUNT];
static uint16_t *s_framebuf;
static lv_color32_t *s_bg_buf;
static uint8_t s_bg_x_from_native_y[BSP_LCD_PANEL_V_RES];
static uint16_t s_bg_row_offset_from_native_x[BSP_LCD_PANEL_H_RES];
static uint16_t s_base_balatro_w;
static uint16_t s_base_balatro_h;
static QueueHandle_t s_free_queue;
static QueueHandle_t s_ready_queue;
static TaskHandle_t s_push_task;
static SemaphoreHandle_t s_push_task_done;
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

static void init_native_scale_maps(void)
{
    uint32_t view_w = ((uint32_t)DIRECT_BG_W * DIRECT_BG_VIEW_W_PCT + 50U) / 100U;
    uint32_t view_h = ((uint32_t)DIRECT_BG_H * DIRECT_BG_VIEW_H_PCT + 50U) / 100U;
    uint32_t margin_x;
    uint32_t margin_y;

    if (view_w == 0 || view_w > DIRECT_BG_W) {
        view_w = DIRECT_BG_W;
    }
    if (view_h == 0 || view_h > DIRECT_BG_H) {
        view_h = DIRECT_BG_H;
    }

    margin_x = (DIRECT_BG_W - view_w) / 2U;
    margin_y = (DIRECT_BG_H - view_h) / 2U;

    for (uint16_t native_y = 0; native_y < BSP_LCD_PANEL_V_RES; native_y++) {
        int32_t logical_x;
        uint32_t bg_x;

#if BSP_UI_ROTATION == BSP_UI_ROTATION_LANDSCAPE_270
        logical_x = native_y;
#else
        logical_x = DIRECT_LOGICAL_W - 1 - native_y;
#endif
        if (logical_x < 0) {
            logical_x = 0;
        }
        if (logical_x >= DIRECT_LOGICAL_W) {
            logical_x = DIRECT_LOGICAL_W - 1;
        }
        bg_x = margin_x + ((uint32_t)logical_x * view_w) / DIRECT_LOGICAL_W;
        if (bg_x >= DIRECT_BG_W) {
            bg_x = DIRECT_BG_W - 1;
        }
        s_bg_x_from_native_y[native_y] = (uint8_t)bg_x;
    }

    for (uint16_t native_x = 0; native_x < BSP_LCD_PANEL_H_RES; native_x++) {
        int32_t logical_y;
        uint32_t bg_y;

#if BSP_UI_ROTATION == BSP_UI_ROTATION_LANDSCAPE_270
        logical_y = DIRECT_LOGICAL_H - 1 - native_x;
#else
        logical_y = native_x;
#endif
        if (logical_y < 0) {
            logical_y = 0;
        }
        if (logical_y >= DIRECT_LOGICAL_H) {
            logical_y = DIRECT_LOGICAL_H - 1;
        }
        bg_y = margin_y + ((uint32_t)logical_y * view_h) / DIRECT_LOGICAL_H;
        if (bg_y >= DIRECT_BG_H) {
            bg_y = DIRECT_BG_H - 1;
        }
        s_bg_row_offset_from_native_x[native_x] = (uint16_t)(bg_y * DIRECT_BG_W);
    }
}

static void put_pixel_logical(int32_t x, int32_t y, uint16_t color)
{
    if (x < 0 || y < 0 || x >= DIRECT_LOGICAL_W || y >= DIRECT_LOGICAL_H || s_framebuf == NULL) {
        return;
    }

    s_framebuf[native_index_from_logical(x, y)] = color;
}

static const uint8_t *find_glyph_rows(char c)
{
    for (size_t i = 0; i < sizeof(s_glyphs) / sizeof(s_glyphs[0]); i++) {
        if (s_glyphs[i].ch == c) {
            return s_glyphs[i].rows;
        }
    }

    return s_glyphs[0].rows;
}

static void draw_char(int32_t x, int32_t y, int32_t scale, char ch, uint16_t color)
{
    const uint8_t *rows = find_glyph_rows(ch);

    for (int32_t row = 0; row < 7; row++) {
        for (int32_t col = 0; col < 5; col++) {
            if ((rows[row] & (1U << (4 - col))) == 0) {
                continue;
            }

            for (int32_t dy = 0; dy < scale; dy++) {
                for (int32_t dx = 0; dx < scale; dx++) {
                    put_pixel_logical(x + col * scale + dx, y + row * scale + dy, color);
                }
            }
        }
    }
}

static void draw_text(int32_t x, int32_t y, int32_t scale, const char *text, uint16_t color)
{
    int32_t cursor = x;

    if (text == NULL) {
        return;
    }

    for (size_t i = 0; text[i] != '\0'; i++) {
        draw_char(cursor, y, scale, text[i], color);
        cursor += 6 * scale;
    }
}

static void fill_background_lowres(uint32_t time_ms)
{
    balatro_render(s_bg_buf, DIRECT_BG_W, time_ms);
}

static void upscale_background_to_native(void)
{
    for (uint16_t native_y = 0; native_y < BSP_LCD_PANEL_V_RES; native_y++) {
        uint16_t *dst = &s_framebuf[(size_t)native_y * BSP_LCD_PANEL_H_RES];
        uint16_t bg_x = s_bg_x_from_native_y[native_y];

        for (uint16_t native_x = 0; native_x < BSP_LCD_PANEL_H_RES; native_x++) {
            lv_color32_t bg = s_bg_buf[s_bg_row_offset_from_native_x[native_x] + bg_x];
            dst[native_x] = rgb565(bg.red, bg.green, bg.blue);
        }
    }
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
    size_t bg_bytes;

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

    bg_bytes = (size_t)DIRECT_BG_W * DIRECT_BG_H * sizeof(lv_color32_t);
    s_bg_buf = heap_caps_malloc(bg_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_bg_buf == NULL) {
        s_bg_buf = heap_caps_malloc(bg_bytes, MALLOC_CAP_8BIT);
    }
    if (s_bg_buf == NULL) {
        screensaver_direct_deinit();
        return false;
    }

    memset(s_bg_buf, 0, bg_bytes);
    balatro_get_dimensions(&s_base_balatro_w, &s_base_balatro_h);
    s_free_queue = xQueueCreate(DIRECT_FRAMEBUF_COUNT, sizeof(uint8_t));
    s_ready_queue = xQueueCreate(DIRECT_FRAMEBUF_COUNT, sizeof(uint8_t));
    s_push_task_done = xSemaphoreCreateBinary();
    if (s_free_queue == NULL || s_ready_queue == NULL || s_push_task_done == NULL) {
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

    init_native_scale_maps();
    screensaver_direct_reset();
    s_ready = true;
    return true;
}

bool screensaver_direct_is_ready(void)
{
    return s_ready && s_framebufs[0] != NULL && s_framebufs[1] != NULL && s_bg_buf != NULL &&
           s_free_queue != NULL && s_ready_queue != NULL && s_push_task != NULL;
}

void screensaver_direct_reset(void)
{
    if (!balatro_init(DIRECT_BG_W, DIRECT_BG_H)) {
        ESP_LOGW(TAG, "balatro direct init failed; renderer will use fallback pattern");
    }
    memset(&s_perf_snapshot, 0, sizeof(s_perf_snapshot));
    s_perf_frames = 0;
    s_perf_compose_total_us = 0;
    s_perf_text_total_us = 0;
    s_perf_wait_total_us = 0;
    s_perf_push_total_us = 0;
    s_perf_window_start_us = 0;
}

void screensaver_direct_restore_background(void)
{
    if (s_base_balatro_w == 0 || s_base_balatro_h == 0) {
        return;
    }
    if (!balatro_init(s_base_balatro_w, s_base_balatro_h)) {
        ESP_LOGW(TAG, "balatro restore init failed for %ux%u", s_base_balatro_w, s_base_balatro_h);
    }
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
    if (s_bg_buf != NULL) {
        heap_caps_free(s_bg_buf);
        s_bg_buf = NULL;
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

    compose_start_us = frame_start_us;
    fill_background_lowres(time_ms);
    upscale_background_to_native();
    compose_us = (uint32_t)(esp_timer_get_time() - compose_start_us);

    text_start_us = esp_timer_get_time();
    draw_text((DIRECT_LOGICAL_W - time_width) / 2, time_y, time_scale,
              (time_text != NULL) ? time_text : "--:--", time_color);
    text_us = (uint32_t)(esp_timer_get_time() - text_start_us);

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
