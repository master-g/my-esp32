#include "bsp_display.h"

#include <assert.h>
#include <string.h>

#include "bsp_board_config.h"
#include "bsp_touch.h"
#include "event_bus.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_axs15231b.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "system_state.h"
#include "ui_theme.h"

extern esp_err_t bsp_backlight_init(void);
extern void bsp_backlight_set_percent(uint8_t percent);

static const char *TAG = "bsp_display";

#define BSP_LCD_BIT_PER_PIXEL 16
#define BSP_LVGL_TICK_MS 5
#define BSP_LVGL_TASK_MAX_DELAY_MS 500
#define BSP_LVGL_TASK_MIN_DELAY_MS 10
#define BSP_LVGL_DIRECT_TASK_MIN_DELAY_MS 1
#define BSP_LVGL_TASK_STACK_SIZE (8 * 1024)
#define BSP_LVGL_TASK_PRIORITY 2
#define BSP_LVGL_DMA_LINES 16
#define BSP_DIRECT_DMA_LINES 64
#define BSP_DMA_BUF_LINES                                                                          \
    ((BSP_DIRECT_DMA_LINES > BSP_LVGL_DMA_LINES) ? BSP_DIRECT_DMA_LINES : BSP_LVGL_DMA_LINES)
#define BSP_LVGL_DMA_BUF_LEN (BSP_LCD_PANEL_H_RES * BSP_DMA_BUF_LINES * 2)
#define BSP_LVGL_FRAME_BUF_SIZE (BSP_LCD_PANEL_H_RES * BSP_LCD_PANEL_V_RES * 2)
#define BSP_EDGE_GESTURE_ZONE_PX 20
#define BSP_EDGE_GESTURE_TRIGGER_PX 72
#define BSP_EDGE_GESTURE_MAX_OFF_AXIS_PX 48

static SemaphoreHandle_t s_lvgl_mutex;
static SemaphoreHandle_t s_flush_done_semaphore;
static esp_lcd_panel_handle_t s_panel;
static lv_display_t *s_display;
static lv_indev_t *s_touch_indev;
static lv_obj_t *s_app_root;
static uint16_t *s_dma_buf;
static uint8_t *s_psram_buf_1;
static uint8_t *s_psram_buf_2;
static uint8_t *s_rot_buf;
static bool s_initialized;
static bsp_display_ui_cb_t s_ui_callback;
static bool s_direct_mode;
static bsp_display_perf_snapshot_t s_perf_snapshot;
static uint16_t s_perf_frames;
static uint64_t s_perf_flush_total_us;
static uint64_t s_perf_rotate_total_us;
static uint64_t s_perf_wait_total_us;
static uint64_t s_perf_push_total_us;
static int64_t s_perf_window_start_us;
static struct {
    bool active;
    app_touch_edge_t edge;
    uint16_t start_x;
    uint16_t start_y;
    uint16_t last_x;
    uint16_t last_y;
} s_touch_gesture;

static const axs15231b_lcd_init_cmd_t s_lcd_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 100},
    {0x29, (uint8_t[]){0x00}, 0, 100},
};

static lv_display_rotation_t get_ui_rotation(void)
{
    switch (BSP_UI_ROTATION) {
    case BSP_UI_ROTATION_LANDSCAPE_270:
        return LV_DISPLAY_ROTATION_270;
    case BSP_UI_ROTATION_LANDSCAPE_90:
    default:
        return LV_DISPLAY_ROTATION_90;
    }
}

static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io,
                                    esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    BaseType_t high_task_awoken = pdFALSE;
    (void)panel_io;
    (void)edata;
    (void)user_ctx;
    xSemaphoreGiveFromISR(s_flush_done_semaphore, &high_task_awoken);
    return false;
}

static int16_t abs_i16(int16_t value) { return (value < 0) ? (int16_t)(-value) : value; }

static app_touch_edge_t gesture_edge_for_x(uint16_t x)
{
    if (x <= BSP_EDGE_GESTURE_ZONE_PX) {
        return APP_TOUCH_EDGE_LEFT;
    }

    if (x >= (BSP_LCD_H_RES - BSP_EDGE_GESTURE_ZONE_PX)) {
        return APP_TOUCH_EDGE_RIGHT;
    }

    return APP_TOUCH_EDGE_NONE;
}

static void transform_touch_to_ui_coords(uint16_t raw_x, uint16_t raw_y, int16_t *ui_x,
                                         int16_t *ui_y)
{
    int32_t x = raw_x;
    int32_t y = raw_y;
    const lv_display_rotation_t rotation =
        (s_display != NULL) ? lv_display_get_rotation(s_display) : LV_DISPLAY_ROTATION_0;

    if (rotation == LV_DISPLAY_ROTATION_180 || rotation == LV_DISPLAY_ROTATION_270) {
        x = BSP_LCD_PANEL_H_RES - x - 1;
        y = BSP_LCD_PANEL_V_RES - y - 1;
    }
    if (rotation == LV_DISPLAY_ROTATION_90 || rotation == LV_DISPLAY_ROTATION_270) {
        int32_t tmp = y;
        y = x;
        x = BSP_LCD_PANEL_V_RES - tmp - 1;
    }

    if (ui_x != NULL) {
        *ui_x = (int16_t)x;
    }
    if (ui_y != NULL) {
        *ui_y = (int16_t)y;
    }
}

static void publish_touch_swipe_event(app_touch_edge_t edge, int16_t delta_x, int16_t delta_y,
                                      uint16_t start_x, uint16_t start_y)
{
    app_touch_event_t touch = {
        .edge = edge,
        .swipe = (delta_x < 0) ? APP_TOUCH_SWIPE_LEFT : APP_TOUCH_SWIPE_RIGHT,
        .delta_x = delta_x,
        .delta_y = delta_y,
        .start_x = start_x,
        .start_y = start_y,
    };
    app_event_t event = {
        .type = APP_EVENT_TOUCH,
        .payload = &touch,
    };

    (void)event_bus_publish(&event);
}

static void maybe_publish_edge_swipe(void)
{
    int16_t delta_x;
    int16_t delta_y;

    if (!s_touch_gesture.active || s_touch_gesture.edge == APP_TOUCH_EDGE_NONE) {
        return;
    }

    delta_x = (int16_t)s_touch_gesture.last_x - (int16_t)s_touch_gesture.start_x;
    delta_y = (int16_t)s_touch_gesture.last_y - (int16_t)s_touch_gesture.start_y;

    if (abs_i16(delta_y) > BSP_EDGE_GESTURE_MAX_OFF_AXIS_PX ||
        abs_i16(delta_x) < BSP_EDGE_GESTURE_TRIGGER_PX || abs_i16(delta_x) <= abs_i16(delta_y)) {
        return;
    }

    if (s_touch_gesture.edge == APP_TOUCH_EDGE_LEFT && delta_x > 0) {
        publish_touch_swipe_event(s_touch_gesture.edge, delta_x, delta_y, s_touch_gesture.start_x,
                                  s_touch_gesture.start_y);
    } else if (s_touch_gesture.edge == APP_TOUCH_EDGE_RIGHT && delta_x < 0) {
        publish_touch_swipe_event(s_touch_gesture.edge, delta_x, delta_y, s_touch_gesture.start_x,
                                  s_touch_gesture.start_y);
    }
}

static uint16_t avg_us_to_ms_x10(uint64_t total_us, uint16_t frames)
{
    if (frames == 0) {
        return 0;
    }

    return (uint16_t)((total_us * 10ULL + ((uint64_t)frames * 500ULL)) /
                      ((uint64_t)frames * 1000ULL));
}

static void note_flush_perf(int64_t flush_start_us, int64_t flush_end_us, uint32_t rotate_us,
                            uint32_t wait_us, uint32_t push_us)
{
    if (s_perf_window_start_us == 0) {
        s_perf_window_start_us = flush_start_us;
    }

    s_perf_frames++;
    s_perf_flush_total_us += (uint64_t)(flush_end_us - flush_start_us);
    s_perf_rotate_total_us += rotate_us;
    s_perf_wait_total_us += wait_us;
    s_perf_push_total_us += push_us;

    if ((flush_end_us - s_perf_window_start_us) >= 500000) {
        s_perf_snapshot.flush_ms_x10 = avg_us_to_ms_x10(s_perf_flush_total_us, s_perf_frames);
        s_perf_snapshot.rotate_ms_x10 = avg_us_to_ms_x10(s_perf_rotate_total_us, s_perf_frames);
        s_perf_snapshot.wait_ms_x10 = avg_us_to_ms_x10(s_perf_wait_total_us, s_perf_frames);
        s_perf_snapshot.push_ms_x10 = avg_us_to_ms_x10(s_perf_push_total_us, s_perf_frames);

        s_perf_frames = 0;
        s_perf_flush_total_us = 0;
        s_perf_rotate_total_us = 0;
        s_perf_wait_total_us = 0;
        s_perf_push_total_us = 0;
        s_perf_window_start_us = flush_end_us;
    }
}

static esp_err_t panel_push_rows_blocking(const uint16_t *map, uint16_t y_offset,
                                          uint16_t total_rows, size_t dma_chunk_rows,
                                          uint32_t *wait_us_out, uint32_t *push_us_out)
{
    uint32_t wait_us = 0;
    uint32_t push_us = 0;
    size_t remaining_rows = total_rows;
    size_t row_offset = y_offset;

    if (dma_chunk_rows == 0 || dma_chunk_rows > BSP_DMA_BUF_LINES) {
        return ESP_ERR_INVALID_ARG;
    }

    if (total_rows == 0) {
        if (wait_us_out != NULL) {
            *wait_us_out = 0;
        }
        if (push_us_out != NULL) {
            *push_us_out = 0;
        }
        return ESP_OK;
    }

    xSemaphoreGive(s_flush_done_semaphore);
    while (remaining_rows > 0) {
        const size_t rows = (remaining_rows > dma_chunk_rows) ? dma_chunk_rows : remaining_rows;
        const size_t byte_count = (rows * BSP_LCD_PANEL_H_RES * sizeof(uint16_t));
        int64_t wait_start_us = esp_timer_get_time();

        if (xSemaphoreTake(s_flush_done_semaphore, pdMS_TO_TICKS(1000)) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
        wait_us += (uint32_t)(esp_timer_get_time() - wait_start_us);

        {
            int64_t push_start_us = esp_timer_get_time();
            memcpy(s_dma_buf, map, byte_count);
            esp_lcd_panel_draw_bitmap(s_panel, 0, row_offset, BSP_LCD_PANEL_H_RES,
                                      row_offset + rows, s_dma_buf);
            push_us += (uint32_t)(esp_timer_get_time() - push_start_us);
        }

        row_offset += rows;
        remaining_rows -= rows;
        map += BSP_LCD_PANEL_H_RES * rows;
    }

    {
        int64_t wait_start_us = esp_timer_get_time();
        if (xSemaphoreTake(s_flush_done_semaphore, pdMS_TO_TICKS(1000)) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
        wait_us += (uint32_t)(esp_timer_get_time() - wait_start_us);
    }

    if (wait_us_out != NULL) {
        *wait_us_out = wait_us;
    }
    if (push_us_out != NULL) {
        *push_us_out = push_us;
    }

    return ESP_OK;
}

static void lvgl_flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *color_p)
{
    int64_t flush_start_us = esp_timer_get_time();
    const lv_display_rotation_t rotation = lv_display_get_rotation(display);
    const lv_color_format_t color_format = lv_display_get_color_format(display);
    const int32_t src_w = lv_display_get_horizontal_resolution(display);
    const int32_t src_h = lv_display_get_vertical_resolution(display);
    const uint32_t src_stride = lv_draw_buf_width_to_stride(src_w, color_format);
    const uint32_t dest_stride = lv_draw_buf_width_to_stride(BSP_LCD_PANEL_H_RES, color_format);
    uint32_t rotate_us = 0;
    uint32_t wait_us = 0;
    uint32_t push_us = 0;
    uint16_t *map = NULL;

    (void)area;
    if (s_direct_mode) {
        lv_display_flush_ready(display);
        return;
    }

    lv_draw_sw_rgb565_swap(color_p, src_w * src_h);

    if (rotation != LV_DISPLAY_ROTATION_0) {
        int64_t rotate_start_us = esp_timer_get_time();
        lv_draw_sw_rotate(color_p, s_rot_buf, src_w, src_h, src_stride, dest_stride, rotation,
                          color_format);
        rotate_us = (uint32_t)(esp_timer_get_time() - rotate_start_us);
        map = (uint16_t *)s_rot_buf;
    } else {
        map = (uint16_t *)color_p;
    }

    {
        esp_err_t err = panel_push_rows_blocking(map, 0, BSP_LCD_PANEL_V_RES, BSP_LVGL_DMA_LINES,
                                                 &wait_us, &push_us);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "DMA flush timeout");
            lv_display_flush_ready(display);
            return;
        }
    }

    note_flush_perf(flush_start_us, esp_timer_get_time(), rotate_us, wait_us, push_us);
    lv_display_flush_ready(display);
}

static void lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    bsp_touch_point_t point = {0};
    int16_t ui_x = 0;
    int16_t ui_y = 0;
    (void)indev;

    if (bsp_touch_read(&point) != ESP_OK || !point.pressed) {
        maybe_publish_edge_swipe();
        memset(&s_touch_gesture, 0, sizeof(s_touch_gesture));
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    transform_touch_to_ui_coords(point.x, point.y, &ui_x, &ui_y);

    if (!s_touch_gesture.active) {
        s_touch_gesture.active = true;
        s_touch_gesture.edge = gesture_edge_for_x((uint16_t)ui_x);
        s_touch_gesture.start_x = (uint16_t)ui_x;
        s_touch_gesture.start_y = (uint16_t)ui_y;
    }

    s_touch_gesture.last_x = (uint16_t)ui_x;
    s_touch_gesture.last_y = (uint16_t)ui_y;
    system_state_note_user_activity();
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = point.x;
    data->point.y = point.y;
}

static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(BSP_LVGL_TICK_MS);
}

static void lvgl_port_task(void *arg)
{
    uint32_t delay_ms = BSP_LVGL_TASK_MAX_DELAY_MS;
    (void)arg;

    for (;;) {
        const uint32_t min_delay_ms =
            s_direct_mode ? BSP_LVGL_DIRECT_TASK_MIN_DELAY_MS : BSP_LVGL_TASK_MIN_DELAY_MS;

        if (bsp_display_lock(100)) {
            if (s_ui_callback != NULL) {
                s_ui_callback();
            }
            delay_ms = lv_timer_handler();
            bsp_display_unlock();
        }

        if (delay_ms > BSP_LVGL_TASK_MAX_DELAY_MS) {
            delay_ms = BSP_LVGL_TASK_MAX_DELAY_MS;
        } else if (delay_ms < min_delay_ms) {
            delay_ms = min_delay_ms;
        }

        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

static esp_err_t init_panel(void)
{
    gpio_config_t gpio_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << BSP_LCD_RST),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    spi_bus_config_t buscfg = {
        .sclk_io_num = BSP_LCD_PCLK,
        .data0_io_num = BSP_LCD_DATA0,
        .data1_io_num = BSP_LCD_DATA1,
        .data2_io_num = BSP_LCD_DATA2,
        .data3_io_num = BSP_LCD_DATA3,
        .max_transfer_sz = BSP_LVGL_DMA_BUF_LEN,
    };
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = BSP_LCD_CS,
        .dc_gpio_num = -1,
        .spi_mode = 3,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .on_color_trans_done = notify_lvgl_flush_ready,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags.quad_mode = true,
    };
    axs15231b_vendor_config_t vendor_config = {
        .flags.use_qspi_interface = 1,
        .init_cmds = s_lcd_init_cmds,
        .init_cmds_size = sizeof(s_lcd_init_cmds) / sizeof(s_lcd_init_cmds[0]),
    };
    esp_lcd_panel_io_handle_t panel_io = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = BSP_LCD_BIT_PER_PIXEL,
        .vendor_config = &vendor_config,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&gpio_conf), TAG, "lcd reset gpio init failed");
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BSP_LCD_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG,
                        "lcd spi init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(BSP_LCD_HOST, &io_config, &panel_io), TAG,
                        "panel io init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_axs15231b(panel_io, &panel_config, &s_panel), TAG,
                        "panel driver init failed");

    ESP_RETURN_ON_ERROR(gpio_set_level(BSP_LCD_RST, 1), TAG, "reset high failed");
    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_RETURN_ON_ERROR(gpio_set_level(BSP_LCD_RST, 0), TAG, "reset low failed");
    vTaskDelay(pdMS_TO_TICKS(250));
    ESP_RETURN_ON_ERROR(gpio_set_level(BSP_LCD_RST, 1), TAG, "reset release failed");
    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init failed");

    return ESP_OK;
}

static esp_err_t init_lvgl(void)
{
    esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &lvgl_tick_cb,
        .name = "bsp_lvgl_tick",
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;

    s_lvgl_mutex = xSemaphoreCreateMutex();
    s_flush_done_semaphore = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_lvgl_mutex != NULL && s_flush_done_semaphore != NULL, ESP_ERR_NO_MEM, TAG,
                        "lvgl sync primitives alloc failed");

    lv_init();
    s_display = lv_display_create(BSP_LCD_PANEL_H_RES, BSP_LCD_PANEL_V_RES);
    ESP_RETURN_ON_FALSE(s_display != NULL, ESP_ERR_NO_MEM, TAG, "display alloc failed");

    s_psram_buf_1 = heap_caps_malloc(BSP_LVGL_FRAME_BUF_SIZE, MALLOC_CAP_SPIRAM);
    s_psram_buf_2 = heap_caps_malloc(BSP_LVGL_FRAME_BUF_SIZE, MALLOC_CAP_SPIRAM);
    s_rot_buf = heap_caps_malloc(BSP_LVGL_FRAME_BUF_SIZE, MALLOC_CAP_SPIRAM);
    s_dma_buf = heap_caps_malloc(BSP_LVGL_DMA_BUF_LEN, MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(s_psram_buf_1 != NULL && s_psram_buf_2 != NULL && s_rot_buf != NULL &&
                            s_dma_buf != NULL,
                        ESP_ERR_NO_MEM, TAG, "display buffers alloc failed");

    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_display, lvgl_flush_cb);
    lv_display_set_buffers(s_display, s_psram_buf_1, s_psram_buf_2, BSP_LVGL_FRAME_BUF_SIZE,
                           LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_user_data(s_display, s_panel);
    lv_display_set_rotation(s_display, get_ui_rotation());
    ui_theme_apply_display(s_display);

    s_touch_indev = lv_indev_create();
    ESP_RETURN_ON_FALSE(s_touch_indev != NULL, ESP_ERR_NO_MEM, TAG, "touch indev alloc failed");
    lv_indev_set_type(s_touch_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_touch_indev, lvgl_touch_read_cb);

    s_app_root = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_app_root);
    lv_obj_set_size(s_app_root, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(s_app_root, ui_theme_color(UI_THEME_COLOR_CANVAS_BG), 0);
    lv_obj_set_style_bg_opa(s_app_root, LV_OPA_COVER, 0);

    ESP_RETURN_ON_ERROR(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer), TAG,
                        "tick timer create failed");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(lvgl_tick_timer, BSP_LVGL_TICK_MS * 1000), TAG,
                        "tick timer start failed");

    {
        BaseType_t ret =
            xTaskCreatePinnedToCore(lvgl_port_task, "bsp_lvgl", BSP_LVGL_TASK_STACK_SIZE, NULL,
                                    BSP_LVGL_TASK_PRIORITY, NULL, 0);
        ESP_RETURN_ON_FALSE(ret == pdPASS, ESP_ERR_NO_MEM, TAG, "lvgl task create failed");
    }

    return ESP_OK;
}

esp_err_t bsp_display_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(bsp_backlight_init(), TAG, "backlight init failed");
    ESP_RETURN_ON_ERROR(init_panel(), TAG, "panel init failed");
    ESP_RETURN_ON_ERROR(init_lvgl(), TAG, "lvgl init failed");
    bsp_backlight_set_percent(100);
    s_initialized = true;
    ESP_LOGI(TAG, "Display + LVGL initialized");
    return ESP_OK;
}

bool bsp_display_lock(uint32_t timeout_ms)
{
    TickType_t timeout_ticks =
        (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);

    if (s_lvgl_mutex == NULL) {
        return false;
    }

    return xSemaphoreTake(s_lvgl_mutex, timeout_ticks) == pdTRUE;
}

void bsp_display_unlock(void)
{
    if (s_lvgl_mutex != NULL) {
        xSemaphoreGive(s_lvgl_mutex);
    }
}

lv_obj_t *bsp_display_get_app_root(void) { return s_app_root; }

void bsp_display_set_backlight_percent(uint8_t percent) { bsp_backlight_set_percent(percent); }

void bsp_display_set_ui_callback(bsp_display_ui_cb_t cb) { s_ui_callback = cb; }

void bsp_display_get_perf_snapshot(bsp_display_perf_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }

    *out = s_perf_snapshot;
}

bool bsp_display_begin_direct_mode(void)
{
    if (!s_initialized || s_panel == NULL || s_flush_done_semaphore == NULL) {
        return false;
    }

    s_direct_mode = true;
    return true;
}

void bsp_display_end_direct_mode(void) { s_direct_mode = false; }

esp_err_t bsp_display_push_native_rgb565(const uint16_t *pixels, uint16_t rows, uint16_t y_offset,
                                         bsp_display_push_stats_t *stats)
{
    esp_err_t err;

    if (!s_direct_mode) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pixels == NULL || rows == 0 || ((uint32_t)y_offset + rows) > BSP_LCD_PANEL_V_RES) {
        return ESP_ERR_INVALID_ARG;
    }

    err = panel_push_rows_blocking(pixels, y_offset, rows, BSP_DIRECT_DMA_LINES,
                                   (stats != NULL) ? &stats->wait_us : NULL,
                                   (stats != NULL) ? &stats->push_us : NULL);
    if (err != ESP_OK && stats != NULL) {
        stats->wait_us = 0;
        stats->push_us = 0;
    }
    return err;
}
