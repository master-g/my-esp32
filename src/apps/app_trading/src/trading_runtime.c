#include "trading_runtime.h"

#include <string.h>

#include "service_market.h"
#include "trading_presenter.h"

static void apply_current_snapshot(trading_runtime_t *runtime)
{
    market_snapshot_t snapshot;
    market_candle_window_t candles = {0};
    trading_present_model_t model;

    if (runtime == NULL || runtime->view.root == NULL) {
        return;
    }

    market_service_get_snapshot(&snapshot);
    (void)market_service_get_candles(snapshot.selection.pair, snapshot.selection.interval,
                                     &candles);
    trading_presenter_build(&model, &snapshot, candles.count > 0 ? &candles : NULL);
    trading_view_apply(&runtime->view, &model);
}

static void pair_btn_cb(lv_event_t *event)
{
    trading_runtime_t *runtime = lv_event_get_user_data(event);
    lv_obj_t *target = lv_event_get_target(event);
    uint32_t i = 0;

    if (runtime == NULL) {
        return;
    }

    for (i = 0; i < MARKET_PAIR_COUNT; ++i) {
        if (runtime->view.pair_buttons[i] == target) {
            market_service_select_pair((market_pair_id_t)i);
            apply_current_snapshot(runtime);
            return;
        }
    }
}

static void interval_btn_cb(lv_event_t *event)
{
    trading_runtime_t *runtime = lv_event_get_user_data(event);
    lv_obj_t *target = lv_event_get_target(event);
    uint32_t i = 0;

    if (runtime == NULL) {
        return;
    }

    for (i = 0; i < MARKET_INTERVAL_COUNT; ++i) {
        if (runtime->view.interval_buttons[i] == target) {
            market_service_select_interval((market_interval_id_t)i);
            apply_current_snapshot(runtime);
            return;
        }
    }
}

esp_err_t trading_runtime_init(trading_runtime_t *runtime)
{
    if (runtime == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(runtime, 0, sizeof(*runtime));
    return ESP_OK;
}

lv_obj_t *trading_runtime_create_root(trading_runtime_t *runtime, lv_obj_t *parent)
{
    uint32_t i = 0;

    if (runtime == NULL) {
        return NULL;
    }

    memset(runtime, 0, sizeof(*runtime));
    trading_view_create(&runtime->view, parent);
    for (i = 0; i < MARKET_PAIR_COUNT; ++i) {
        lv_obj_add_event_cb(runtime->view.pair_buttons[i], pair_btn_cb, LV_EVENT_CLICKED, runtime);
    }
    for (i = 0; i < MARKET_INTERVAL_COUNT; ++i) {
        lv_obj_add_event_cb(runtime->view.interval_buttons[i], interval_btn_cb, LV_EVENT_CLICKED,
                            runtime);
    }

    apply_current_snapshot(runtime);
    return runtime->view.root;
}

void trading_runtime_resume(trading_runtime_t *runtime)
{
    if (runtime == NULL) {
        return;
    }

    trading_view_set_hidden(&runtime->view, false);
    apply_current_snapshot(runtime);
}

void trading_runtime_suspend(trading_runtime_t *runtime)
{
    if (runtime == NULL) {
        return;
    }

    trading_view_set_hidden(&runtime->view, true);
}

void trading_runtime_handle_event(trading_runtime_t *runtime, const app_event_t *event)
{
    if (runtime == NULL || event == NULL) {
        return;
    }

    switch (event->type) {
    case APP_EVENT_ENTER:
    case APP_EVENT_TICK_1S:
    case APP_EVENT_NET_CHANGED:
    case APP_EVENT_POWER_CHANGED:
    case APP_EVENT_DATA_MARKET:
        apply_current_snapshot(runtime);
        break;
    default:
        break;
    }
}
