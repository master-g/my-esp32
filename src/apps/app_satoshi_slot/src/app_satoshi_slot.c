#include "app_satoshi_slot.h"

#include "bsp_board_config.h"
#include "lvgl.h"
#include "ui_fonts.h"

#define APP_SLOT_BODY_Y 44

static lv_obj_t *s_root;

static esp_err_t app_satoshi_slot_init(void) { return ESP_OK; }

static lv_obj_t *app_satoshi_slot_create_root(lv_obj_t *parent)
{
    lv_obj_t *title = NULL;
    lv_obj_t *body = NULL;

    s_root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(0x20140e), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_root, 16, 0);

    title = lv_label_create(s_root);
    lv_label_set_text(title, "Satoshi Slot");
    lv_obj_set_style_text_font(title, ui_font_text_22(), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffd08a), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    body = lv_label_create(s_root);
    lv_obj_set_width(body, BSP_LCD_H_RES - 32);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(body, ui_font_text_11(), 0);
    lv_label_set_text(
        body, "Compute path reserved.\nPolicy wiring is active,\nalgorithm not yet attached.");
    lv_obj_set_style_text_color(body, lv_color_hex(0xf1d8b5), 0);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, APP_SLOT_BODY_Y);
    return s_root;
}

static void app_satoshi_slot_handle_event(const app_event_t *event) { (void)event; }

const app_descriptor_t *app_satoshi_slot_get_descriptor(void)
{
    static const app_descriptor_t descriptor = {
        .id = APP_ID_SATOSHI_SLOT,
        .name = "Satoshi Slot",
        .init = app_satoshi_slot_init,
        .create_root = app_satoshi_slot_create_root,
        .resume = NULL,
        .suspend = NULL,
        .handle_event = app_satoshi_slot_handle_event,
    };

    return &descriptor;
}
