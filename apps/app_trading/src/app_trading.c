#include "app_trading.h"

#include "bsp_board_config.h"
#include "lvgl.h"
#include "power_policy.h"
#include "service_market.h"

static lv_obj_t *s_root;
static lv_obj_t *s_status;

static esp_err_t app_trading_init(void)
{
    return ESP_OK;
}

static lv_obj_t *app_trading_create_root(lv_obj_t *parent)
{
    lv_obj_t *title = NULL;

    s_root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(0x131913), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_root, 16, 0);

    title = lv_label_create(s_root);
    lv_label_set_text(title, "Trading");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xe7ffd7), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_status = lv_label_create(s_root);
    lv_obj_set_width(s_status, BSP_LCD_H_RES - 32);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_status, lv_color_hex(0xcdd9cd), 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 0, 56);
    lv_label_set_text(s_status, "Market service placeholder");
    return s_root;
}

static void app_trading_resume(void)
{
    if (power_policy_is_refresh_mode(REFRESH_MODE_REALTIME, APP_ID_TRADING)) {
        market_service_on_refresh_mode_changed(REFRESH_MODE_REALTIME);
    }
}

static void app_trading_handle_event(const app_event_t *event)
{
    const power_policy_output_t *policy = NULL;

    if (event == NULL) {
        return;
    }

    if (event->type == APP_EVENT_POWER_CHANGED) {
        policy = power_policy_get_output();
        market_service_on_refresh_mode_changed(policy->market_mode);
    }

    if ((event->type == APP_EVENT_ENTER || event->type == APP_EVENT_POWER_CHANGED) && s_status != NULL) {
        const market_snapshot_t *snapshot = market_service_get_snapshot();
        lv_label_set_text_fmt(s_status,
                              "%s\nprice=%s\nmode=%d",
                              snapshot->pair_label,
                              snapshot->price_text,
                              power_policy_get_output()->market_mode);
    }
}

const app_descriptor_t *app_trading_get_descriptor(void)
{
    static const app_descriptor_t descriptor = {
        .id = APP_ID_TRADING,
        .name = "Trading",
        .init = app_trading_init,
        .create_root = app_trading_create_root,
        .resume = app_trading_resume,
        .suspend = NULL,
        .handle_event = app_trading_handle_event,
    };

    return &descriptor;
}
