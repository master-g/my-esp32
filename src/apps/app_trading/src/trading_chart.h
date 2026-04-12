#ifndef APP_TRADING_CHART_H
#define APP_TRADING_CHART_H

#include "lvgl.h"
#include "trading_presenter.h"

typedef struct {
    lv_obj_t *root;
    lv_obj_t *hint_label;
    lv_obj_t *grid_lines[3];
    lv_obj_t *wicks[MARKET_MAX_CANDLES];
    lv_obj_t *bodies[MARKET_MAX_CANDLES];
} trading_chart_t;

lv_obj_t *trading_chart_create(trading_chart_t *chart, lv_obj_t *parent);
void trading_chart_apply(trading_chart_t *chart, const trading_present_model_t *model);

#endif
