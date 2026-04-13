#include "trading_chart.h"

#include <string.h>

#include "ui_theme.h"
#include "ui_fonts.h"

#define TRADING_CHART_GRID ui_theme_color_hex(UI_THEME_COLOR_BORDER_SUBTLE)
#define TRADING_CHART_UP ui_theme_color_hex(UI_THEME_COLOR_STATUS_SUCCESS)
#define TRADING_CHART_DOWN ui_theme_color_hex(UI_THEME_COLOR_STATUS_ERROR)
#define TRADING_CHART_NEUTRAL ui_theme_color_hex(UI_THEME_COLOR_TEXT_SECONDARY)

static lv_obj_t *create_plot_bar(lv_obj_t *parent, uint32_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);

    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    return obj;
}

lv_obj_t *trading_chart_create(trading_chart_t *chart, lv_obj_t *parent)
{
    uint32_t i = 0;

    if (chart == NULL) {
        return NULL;
    }

    memset(chart, 0, sizeof(*chart));
    chart->root = lv_obj_create(parent);
    lv_obj_remove_style_all(chart->root);
    lv_obj_clear_flag(chart->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(chart->root, LV_OPA_TRANSP, 0);

    for (i = 0; i < 3U; ++i) {
        chart->grid_lines[i] = lv_obj_create(chart->root);
        lv_obj_remove_style_all(chart->grid_lines[i]);
        lv_obj_set_style_bg_color(chart->grid_lines[i], lv_color_hex(TRADING_CHART_GRID), 0);
        lv_obj_set_style_bg_opa(chart->grid_lines[i], LV_OPA_40, 0);
        lv_obj_clear_flag(chart->grid_lines[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    for (i = 0; i < MARKET_MAX_CANDLES; ++i) {
        chart->wicks[i] = create_plot_bar(chart->root, TRADING_CHART_NEUTRAL);
        chart->bodies[i] = create_plot_bar(chart->root, TRADING_CHART_NEUTRAL);
    }

    chart->hint_label = lv_label_create(chart->root);
    lv_obj_set_style_text_font(chart->hint_label, ui_font_text_11(), 0);
    lv_obj_set_style_text_color(chart->hint_label, ui_theme_color(UI_THEME_COLOR_TEXT_MUTED), 0);
    lv_label_set_long_mode(chart->hint_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(chart->hint_label, lv_pct(100));
    lv_obj_center(chart->hint_label);
    lv_obj_set_style_text_align(chart->hint_label, LV_TEXT_ALIGN_CENTER, 0);
    return chart->root;
}

void trading_chart_apply(trading_chart_t *chart, const trading_present_model_t *model)
{
    int32_t min_price = 0;
    int32_t max_price = 0;
    int32_t range = 0;
    int32_t pad = 0;
    int32_t width = 0;
    int32_t height = 0;
    int32_t plot_x = 3;
    int32_t plot_y = 4;
    int32_t plot_w = 0;
    int32_t plot_h = 0;
    int32_t step = 0;
    int32_t body_w = 0;
    int32_t wick_w = 0;
    uint32_t i = 0;
    lv_opa_t bar_opa = model->chart_dimmed ? LV_OPA_50 : LV_OPA_COVER;
    lv_opa_t grid_opa = model->chart_dimmed ? LV_OPA_20 : LV_OPA_40;

    if (chart == NULL || model == NULL || chart->root == NULL) {
        return;
    }

    width = lv_obj_get_width(chart->root);
    height = lv_obj_get_height(chart->root);
    plot_w = width - (plot_x * 2);
    plot_h = height - (plot_y * 2);

    for (i = 0; i < 3U; ++i) {
        lv_obj_set_size(chart->grid_lines[i], plot_w, 1);
        lv_obj_set_pos(chart->grid_lines[i], plot_x, plot_y + ((int32_t)i * (plot_h - 1)) / 2);
        lv_obj_set_style_bg_opa(chart->grid_lines[i], grid_opa, 0);
    }

    if (!model->has_chart_data || model->candles.count == 0) {
        lv_label_set_text(chart->hint_label, model->chart_status_text);
        lv_obj_center(chart->hint_label);
        lv_obj_clear_flag(chart->hint_label, LV_OBJ_FLAG_HIDDEN);
        for (i = 0; i < MARKET_MAX_CANDLES; ++i) {
            lv_obj_add_flag(chart->wicks[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(chart->bodies[i], LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    lv_obj_add_flag(chart->hint_label, LV_OBJ_FLAG_HIDDEN);
    min_price = model->candles.candles[0].low_scaled;
    max_price = model->candles.candles[0].high_scaled;
    for (i = 1; i < model->candles.count; ++i) {
        if (model->candles.candles[i].low_scaled < min_price) {
            min_price = model->candles.candles[i].low_scaled;
        }
        if (model->candles.candles[i].high_scaled > max_price) {
            max_price = model->candles.candles[i].high_scaled;
        }
    }

    range = max_price - min_price;
    if (range <= 0) {
        range = 100;
    }
    pad = range / 12;
    if (pad < 10) {
        pad = 10;
    }
    max_price += pad;
    min_price -= pad;
    range = max_price - min_price;

    step = plot_w / (int32_t)model->candles.count;
    if (step < 6) {
        step = 6;
    }
    body_w = (step / 2) + 1;
    if (body_w < 3) {
        body_w = 3;
    } else if (body_w > 10) {
        body_w = 10;
    }
    wick_w = (body_w >= 5) ? 2 : 1;

    for (i = 0; i < MARKET_MAX_CANDLES; ++i) {
        if (i >= model->candles.count) {
            lv_obj_add_flag(chart->wicks[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(chart->bodies[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        {
            const market_candle_t *candle = &model->candles.candles[i];
            int32_t x_center = plot_x + ((int32_t)i * step) + (step / 2);
            int32_t high_y = plot_y + (((max_price - candle->high_scaled) * plot_h) / range);
            int32_t low_y = plot_y + (((max_price - candle->low_scaled) * plot_h) / range);
            int32_t open_y = plot_y + (((max_price - candle->open_scaled) * plot_h) / range);
            int32_t close_y = plot_y + (((max_price - candle->close_scaled) * plot_h) / range);
            int32_t body_y = (open_y < close_y) ? open_y : close_y;
            int32_t body_h = (open_y > close_y) ? (open_y - close_y) : (close_y - open_y);
            uint32_t color = TRADING_CHART_NEUTRAL;

            if (body_h < 2) {
                body_h = 2;
            }

            if (candle->close_scaled > candle->open_scaled) {
                color = TRADING_CHART_UP;
            } else if (candle->close_scaled < candle->open_scaled) {
                color = TRADING_CHART_DOWN;
            }

            lv_obj_clear_flag(chart->wicks[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(chart->wicks[i], x_center - (wick_w / 2), high_y);
            lv_obj_set_size(chart->wicks[i], wick_w, LV_MAX(2, low_y - high_y));
            lv_obj_set_style_bg_color(chart->wicks[i], lv_color_hex(color), 0);
            lv_obj_set_style_bg_opa(chart->wicks[i], bar_opa, 0);

            lv_obj_clear_flag(chart->bodies[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(chart->bodies[i], x_center - (body_w / 2), body_y);
            lv_obj_set_size(chart->bodies[i], body_w, body_h);
            lv_obj_set_style_bg_color(chart->bodies[i], lv_color_hex(color), 0);
            lv_obj_set_style_bg_opa(chart->bodies[i], bar_opa, 0);
            lv_obj_set_style_radius(chart->bodies[i], 1, 0);
        }
    }
}
