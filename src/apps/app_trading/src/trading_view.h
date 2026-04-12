#ifndef APP_TRADING_VIEW_H
#define APP_TRADING_VIEW_H

#include <stdbool.h>

#include "lvgl.h"
#include "trading_chart.h"
#include "trading_presenter.h"

typedef struct {
    lv_obj_t *root;
    lv_obj_t *pair_buttons[MARKET_PAIR_COUNT];
    lv_obj_t *pair_labels[MARKET_PAIR_COUNT];
    lv_obj_t *transport_chip;
    lv_obj_t *transport_label;
    lv_obj_t *price_label;
    lv_obj_t *change_label;
    lv_obj_t *status_label;
    lv_obj_t *chart_panel;
    lv_obj_t *interval_buttons[MARKET_INTERVAL_COUNT];
    lv_obj_t *interval_labels[MARKET_INTERVAL_COUNT];
    trading_chart_t chart;
} trading_view_t;

lv_obj_t *trading_view_create(trading_view_t *view, lv_obj_t *parent);
void trading_view_apply(trading_view_t *view, const trading_present_model_t *model);
void trading_view_set_hidden(trading_view_t *view, bool hidden);

#endif
