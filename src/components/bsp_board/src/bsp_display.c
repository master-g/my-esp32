#include "bsp_display.h"

#include <assert.h>
#include <string.h>

#include "bsp_board_config.h"
#include "bsp_touch.h"
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

extern esp_err_t bsp_backlight_init(void);
extern void bsp_backlight_set_percent(uint8_t percent);

static const char *TAG = "bsp_display";

#define BSP_LCD_BIT_PER_PIXEL 16
#define BSP_LVGL_TICK_MS 5
#define BSP_LVGL_TASK_MAX_DELAY_MS 500
#define BSP_LVGL_TASK_MIN_DELAY_MS 10
#define BSP_LVGL_TASK_STACK_SIZE (8 * 1024)
#define BSP_LVGL_TASK_PRIORITY 2
#define BSP_LVGL_DMA_LINES 16
#define BSP_LVGL_DMA_BUF_LEN (BSP_LCD_PANEL_H_RES * BSP_LVGL_DMA_LINES * 2)
#define BSP_LVGL_FRAME_BUF_SIZE (BSP_LCD_PANEL_H_RES * BSP_LCD_PANEL_V_RES * 2)

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

static void lvgl_flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *color_p)
{
    const lv_display_rotation_t rotation = lv_display_get_rotation(display);
    const lv_color_format_t color_format = lv_display_get_color_format(display);
    const int32_t src_w = lv_display_get_horizontal_resolution(display);
    const int32_t src_h = lv_display_get_vertical_resolution(display);
    const uint32_t src_stride = lv_draw_buf_width_to_stride(src_w, color_format);
    const uint32_t dest_stride = lv_draw_buf_width_to_stride(BSP_LCD_PANEL_H_RES, color_format);
    const size_t dma_chunk_rows = (BSP_LVGL_DMA_BUF_LEN / (BSP_LCD_PANEL_H_RES * sizeof(uint16_t)));
    size_t remaining_rows = BSP_LCD_PANEL_V_RES;
    size_t row_offset = 0;
    uint16_t *map = NULL;

    (void)area;
    lv_draw_sw_rgb565_swap(color_p, src_w * src_h);

    if (rotation != LV_DISPLAY_ROTATION_0) {
        lv_draw_sw_rotate(color_p, s_rot_buf, src_w, src_h, src_stride, dest_stride, rotation,
                          color_format);
        map = (uint16_t *)s_rot_buf;
    } else {
        map = (uint16_t *)color_p;
    }

    xSemaphoreGive(s_flush_done_semaphore);
    while (remaining_rows > 0) {
        const size_t rows = (remaining_rows > dma_chunk_rows) ? dma_chunk_rows : remaining_rows;
        const size_t byte_count = (rows * BSP_LCD_PANEL_H_RES * sizeof(uint16_t));

        xSemaphoreTake(s_flush_done_semaphore, portMAX_DELAY);
        memcpy(s_dma_buf, map, byte_count);
        esp_lcd_panel_draw_bitmap((esp_lcd_panel_handle_t)lv_display_get_user_data(display), 0,
                                  row_offset, BSP_LCD_PANEL_H_RES, row_offset + rows, s_dma_buf);
        row_offset += rows;
        remaining_rows -= rows;
        map += BSP_LCD_PANEL_H_RES * rows;
    }

    xSemaphoreTake(s_flush_done_semaphore, portMAX_DELAY);
    lv_display_flush_ready(display);
}

static void lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    bsp_touch_point_t point = {0};
    (void)indev;

    if (bsp_touch_read(&point) != ESP_OK || !point.pressed) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

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
        if (bsp_display_lock(UINT32_MAX)) {
            delay_ms = lv_timer_handler();
            bsp_display_unlock();
        }

        if (delay_ms > BSP_LVGL_TASK_MAX_DELAY_MS) {
            delay_ms = BSP_LVGL_TASK_MAX_DELAY_MS;
        } else if (delay_ms < BSP_LVGL_TASK_MIN_DELAY_MS) {
            delay_ms = BSP_LVGL_TASK_MIN_DELAY_MS;
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

    s_touch_indev = lv_indev_create();
    ESP_RETURN_ON_FALSE(s_touch_indev != NULL, ESP_ERR_NO_MEM, TAG, "touch indev alloc failed");
    lv_indev_set_type(s_touch_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_touch_indev, lvgl_touch_read_cb);

    s_app_root = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_app_root);
    lv_obj_set_size(s_app_root, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(s_app_root, lv_color_hex(0x11161c), 0);
    lv_obj_set_style_bg_opa(s_app_root, LV_OPA_COVER, 0);

    ESP_RETURN_ON_ERROR(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer), TAG,
                        "tick timer create failed");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(lvgl_tick_timer, BSP_LVGL_TICK_MS * 1000), TAG,
                        "tick timer start failed");

    xTaskCreatePinnedToCore(lvgl_port_task, "bsp_lvgl", BSP_LVGL_TASK_STACK_SIZE, NULL,
                            BSP_LVGL_TASK_PRIORITY, NULL, 0);

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
