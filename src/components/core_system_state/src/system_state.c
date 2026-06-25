#include "system_state.h"

#include <string.h>

#include "core_types/app_event.h"
#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "power_policy.h"

static power_policy_input_t s_input;
static uint32_t s_user_activity_seq;
static SemaphoreHandle_t s_mutex;
static bool s_initialized;

// 在持 s_mutex 时调用:返回策略输入是否有变化。
// 故意不在此发事件——event_bus_publish 同步派发给订阅者,若订阅者回调反向调
// system_state_set_*(),持锁内 publish 会在二次 take(s_mutex) 处死锁。改为算完放锁后再发。
static bool recompute_locked(void)
{
    return s_initialized && power_policy_on_input_changed(&s_input);
}

// 在 s_mutex 已释放后调用,发布 power-changed 信号。
// payload 为 NULL,遵循事件总线约定:订阅者以事件类型为信号,自行 query 最新状态。
static void publish_power_changed(void)
{
    app_event_t event = {
        .type = APP_EVENT_POWER_CHANGED,
        .payload = NULL,
    };
    event_bus_publish(&event);
}

esp_err_t system_state_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memset(&s_input, 0, sizeof(s_input));
    s_input.power_source = POWER_SOURCE_USB;
    s_input.display_state = DISPLAY_STATE_ACTIVE;
    s_input.foreground_app = APP_ID_HOME;
    s_user_activity_seq = 0;
    s_initialized = true;

    // init 跑在启动期单线程,无竞争;直接算并发布初始状态。
    if (recompute_locked()) {
        publish_power_changed();
    }
    return ESP_OK;
}

void system_state_get_power_policy_input(power_policy_input_t *out)
{
    if (out == NULL) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_input;
    xSemaphoreGive(s_mutex);
}

uint32_t system_state_get_user_activity_seq(void)
{
    uint32_t seq = 0;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    seq = s_user_activity_seq;
    xSemaphoreGive(s_mutex);
    return seq;
}

void system_state_set_power_source(power_source_t power_source)
{
    bool changed;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_input.power_source = power_source;
    changed = recompute_locked();
    xSemaphoreGive(s_mutex);

    if (changed) {
        publish_power_changed();
    }
}

void system_state_set_display_state(display_state_t display_state)
{
    bool changed;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_input.display_state = display_state;
    changed = recompute_locked();
    xSemaphoreGive(s_mutex);

    if (changed) {
        publish_power_changed();
    }
}

void system_state_set_foreground_app(app_id_t app_id)
{
    bool changed;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_input.foreground_app = app_id;
    changed = recompute_locked();
    xSemaphoreGive(s_mutex);

    if (changed) {
        publish_power_changed();
    }
}

void system_state_set_wifi_connected(bool wifi_connected)
{
    bool changed;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_input.wifi_connected = wifi_connected;
    changed = recompute_locked();
    xSemaphoreGive(s_mutex);

    if (changed) {
        publish_power_changed();
    }
}

void system_state_set_user_interacting(bool user_interacting)
{
    bool changed;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_input.user_interacting = user_interacting;
    changed = recompute_locked();
    xSemaphoreGive(s_mutex);

    if (changed) {
        publish_power_changed();
    }
}

void system_state_note_user_activity(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_user_activity_seq++;
    xSemaphoreGive(s_mutex);
}
