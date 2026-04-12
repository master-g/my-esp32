#include "bootstrap.h"

#include <stdbool.h>
#include <stddef.h>

#include "app_home.h"
#include "app_manager.h"
#include "app_settings.h"
#include "app_satoshi_slot.h"
#include "app_trading.h"
#include "bsp_board.h"
#include "bsp_board_config.h"
#include "bsp_display.h"
#include "device_link.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "event_bus.h"
#include "net_manager.h"
#include "nvs_flash.h"
#include "power_policy.h"
#include "power_runtime.h"
#include "service_claude.h"
#include "service_bitcoin.h"
#include "service_home.h"
#include "service_market.h"
#include "service_settings.h"
#include "service_time.h"
#include "service_weather.h"
#include "system_state.h"

#if CONFIG_NVS_ENCRYPTION && CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC &&                              \
    (CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID < 0)
#error "CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID must be set when HMAC-based NVS encryption is enabled."
#endif

static const char *TAG = "bootstrap";
static esp_timer_handle_t s_tick_timer;

static void tick_1s_cb(void *arg)
{
    app_event_t event = {
        .type = APP_EVENT_TICK_1S,
        .payload = NULL,
    };

    (void)arg;
    claude_service_check_staleness();
    event_bus_publish(&event);
}

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    ESP_RETURN_ON_ERROR(err, TAG, "default nvs init failed");

#if CONFIG_NVS_ENCRYPTION
    ESP_RETURN_ON_FALSE(nvs_flash_get_default_security_scheme() != NULL, ESP_FAIL, TAG,
                        "encrypted nvs requested but no security scheme is active");
#if CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC
    ESP_LOGI(TAG,
             "Encrypted NVS enabled via HMAC provider (eFuse key slot %d; first boot may burn it "
             "if empty)",
             CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID);
#elif CONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC
    ESP_LOGI(TAG, "Encrypted NVS enabled via flash encryption");
#else
    ESP_LOGI(TAG, "Encrypted NVS enabled via custom provider");
#endif
#else
    ESP_LOGW(TAG, "NVS encryption is disabled; secure storage features stay blocked");
#endif

    return ESP_OK;
}

static esp_err_t register_apps(void)
{
    const app_descriptor_t *descriptors[] = {
        app_home_get_descriptor(),
        app_trading_get_descriptor(),
        app_satoshi_slot_get_descriptor(),
        app_settings_get_descriptor(),
    };
    size_t i = 0;

    for (i = 0; i < sizeof(descriptors) / sizeof(descriptors[0]); ++i) {
        ESP_RETURN_ON_ERROR(app_manager_register(descriptors[i]), TAG, "failed to register app %u",
                            (unsigned)i);
    }

    return ESP_OK;
}

esp_err_t bootstrap_start(void)
{
    ESP_LOGI(TAG, "Starting firmware scaffold for %s", BSP_BOARD_NAME);
    ESP_LOGI(TAG, "Board baseline: panel=%ux%u, ui=%ux%u landscape, LCD host=%d, touch addr=0x%02x",
             BSP_LCD_PANEL_H_RES, BSP_LCD_PANEL_V_RES, BSP_LCD_H_RES, BSP_LCD_V_RES, BSP_LCD_HOST,
             BSP_TOUCH_I2C_ADDR);

    ESP_RETURN_ON_ERROR(init_nvs(), TAG, "nvs init failed");
    ESP_RETURN_ON_ERROR(event_bus_init(), TAG, "event bus init failed");
    ESP_RETURN_ON_ERROR(app_manager_init(), TAG, "app manager init failed");
    ESP_RETURN_ON_ERROR(event_bus_subscribe(app_manager_on_event, NULL), TAG,
                        "failed to subscribe app manager");
    ESP_RETURN_ON_ERROR(power_policy_init(), TAG, "power policy init failed");
    ESP_RETURN_ON_ERROR(system_state_init(), TAG, "system state init failed");
    ESP_RETURN_ON_ERROR(bsp_board_init(), TAG, "board init failed");
    bsp_display_set_ui_callback(app_manager_process_ui_events);
    ESP_RETURN_ON_ERROR(power_runtime_init(), TAG, "power runtime init failed");
    ESP_RETURN_ON_ERROR(net_manager_init(), TAG, "net manager init failed");
    ESP_RETURN_ON_ERROR(time_service_init(), TAG, "time service init failed");
    ESP_RETURN_ON_ERROR(weather_service_init(), TAG, "weather service init failed");
    ESP_RETURN_ON_ERROR(claude_service_init(), TAG, "claude service init failed");
    ESP_RETURN_ON_ERROR(market_service_init(), TAG, "market service init failed");
    ESP_RETURN_ON_ERROR(bitcoin_service_init(), TAG, "bitcoin service init failed");
    ESP_RETURN_ON_ERROR(home_service_init(), TAG, "home service init failed");
    ESP_RETURN_ON_ERROR(settings_service_init(), TAG, "settings service init failed");
    ESP_RETURN_ON_ERROR(net_manager_start(), TAG, "net manager start failed");
    ESP_RETURN_ON_ERROR(time_service_start(), TAG, "time service start failed");
    ESP_RETURN_ON_ERROR(device_link_init(), TAG, "device link init failed");
    ESP_RETURN_ON_ERROR(register_apps(), TAG, "app registration failed");

    {
        esp_timer_create_args_t tick_timer_args = {
            .callback = tick_1s_cb,
            .name = "app_tick_1s",
        };
        ESP_RETURN_ON_ERROR(esp_timer_create(&tick_timer_args, &s_tick_timer), TAG,
                            "tick timer create failed");
        ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_tick_timer, 1000000), TAG,
                            "tick timer start failed");
    }

    ESP_RETURN_ON_ERROR(app_manager_switch_to(APP_ID_HOME), TAG, "failed to switch to home");

    ESP_LOGI(TAG, "Firmware scaffold initialized");
    return ESP_OK;
}
