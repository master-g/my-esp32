#include "net_manager.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "core_types/app_event.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_wifi.h"
#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "nvs.h"
#include "system_state.h"

#define NET_NVS_NAMESPACE "wifi"
#define NET_NVS_KEY_SSID "ssid"
#define NET_NVS_KEY_PASSWORD "password"
#define NET_FAST_RETRY_MS 2000
#define NET_SLOW_RETRY_MS 10000
#define NET_FAST_RETRY_COUNT 3

static const char *TAG = "net_manager";
static net_snapshot_t s_snapshot;
static bool s_started;
static bool s_wifi_stack_ready;
static esp_netif_t *s_sta_netif;
static esp_event_handler_instance_t s_wifi_event_instance;
static esp_event_handler_instance_t s_ip_event_instance;
static TimerHandle_t s_retry_timer;
static uint32_t s_disconnect_count;
static char s_wifi_password[65];

static bool has_text(const char *text)
{
    return text != NULL && text[0] != '\0';
}

static void publish_net_event(void)
{
    app_event_t event = {
        .type = APP_EVENT_NET_CHANGED,
        .payload = &s_snapshot,
    };

    event_bus_publish(&event);
}

static size_t bounded_cstr_len(const char *text, size_t max_len)
{
    size_t len = 0;

    while (len < max_len && text[len] != '\0') {
        len++;
    }

    return len;
}

static bool is_auth_failure_reason(wifi_err_reason_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        return true;
    default:
        return false;
    }
}

static esp_err_t read_nvs_string(nvs_handle_t handle, const char *key, char *buffer, size_t buffer_len)
{
    size_t required = buffer_len;
    esp_err_t err = nvs_get_str(handle, key, buffer, &required);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        buffer[0] = '\0';
        return ESP_OK;
    }

    return err;
}

static esp_err_t seed_credentials_from_defaults(nvs_handle_t handle)
{
    bool changed = false;

    if (!has_text(s_snapshot.ssid) && has_text(CONFIG_DASH_WIFI_SSID)) {
        ESP_RETURN_ON_ERROR(nvs_set_str(handle, NET_NVS_KEY_SSID, CONFIG_DASH_WIFI_SSID), TAG, "failed to store default ssid");
        changed = true;
    }

    if (!has_text(s_wifi_password) && has_text(CONFIG_DASH_WIFI_PASSWORD)) {
        ESP_RETURN_ON_ERROR(nvs_set_str(handle, NET_NVS_KEY_PASSWORD, CONFIG_DASH_WIFI_PASSWORD),
                            TAG,
                            "failed to store default password");
        changed = true;
    }

    if (changed) {
        ESP_RETURN_ON_ERROR(nvs_commit(handle), TAG, "failed to commit default credentials");
        ESP_LOGI(TAG, "Seeded Wi-Fi credentials from build-time defaults");
    }

    return ESP_OK;
}

static esp_err_t load_credentials_from_nvs(void)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NET_NVS_NAMESPACE, NVS_READWRITE, &handle);

    ESP_RETURN_ON_ERROR(err, TAG, "failed to open wifi namespace");

    memset(s_snapshot.ssid, 0, sizeof(s_snapshot.ssid));
    memset(s_wifi_password, 0, sizeof(s_wifi_password));

    err = read_nvs_string(handle, NET_NVS_KEY_SSID, s_snapshot.ssid, sizeof(s_snapshot.ssid));
    if (err == ESP_OK) {
        err = read_nvs_string(handle, NET_NVS_KEY_PASSWORD, s_wifi_password, sizeof(s_wifi_password));
    }
    if (err == ESP_OK) {
        err = seed_credentials_from_defaults(handle);
    }
    if (err == ESP_OK && !has_text(s_snapshot.ssid) && has_text(CONFIG_DASH_WIFI_SSID)) {
        snprintf(s_snapshot.ssid, sizeof(s_snapshot.ssid), "%s", CONFIG_DASH_WIFI_SSID);
    }
    if (err == ESP_OK && !has_text(s_wifi_password) && has_text(CONFIG_DASH_WIFI_PASSWORD)) {
        snprintf(s_wifi_password, sizeof(s_wifi_password), "%s", CONFIG_DASH_WIFI_PASSWORD);
    }

    nvs_close(handle);
    ESP_RETURN_ON_ERROR(err, TAG, "failed loading Wi-Fi credentials");

    s_snapshot.has_credentials = has_text(s_snapshot.ssid);
    return ESP_OK;
}

static void retry_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    if (s_started && s_snapshot.has_credentials) {
        esp_wifi_connect();
    }
}

static void schedule_retry(void)
{
    TickType_t delay_ticks;

    if (s_retry_timer == NULL || !s_snapshot.has_credentials) {
        return;
    }

    delay_ticks = pdMS_TO_TICKS((s_disconnect_count < NET_FAST_RETRY_COUNT) ? NET_FAST_RETRY_MS : NET_SLOW_RETRY_MS);
    s_disconnect_count++;
    xTimerStop(s_retry_timer, 0);
    xTimerChangePeriod(s_retry_timer, delay_ticks, 0);
    xTimerStart(s_retry_timer, 0);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        s_snapshot.state = NET_STATE_CONNECTING;
        publish_net_event();
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = (const wifi_event_sta_disconnected_t *)event_data;

        s_snapshot.state = s_snapshot.has_credentials ? NET_STATE_CONNECTING : NET_STATE_DOWN;
        s_snapshot.wifi_connected = false;
        s_snapshot.ip_ready = false;
        s_snapshot.auth_failed = is_auth_failure_reason(event->reason);
        s_snapshot.last_disconnect_reason = event->reason;
        s_snapshot.ip_addr[0] = '\0';
        system_state_set_wifi_connected(false);
        publish_net_event();
        schedule_retry();
        ESP_LOGW(TAG, "Wi-Fi disconnected, reason=%" PRIi32, (int32_t)event->reason);
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;

        s_disconnect_count = 0;
        s_snapshot.state = NET_STATE_UP;
        s_snapshot.wifi_connected = true;
        s_snapshot.ip_ready = true;
        s_snapshot.auth_failed = false;
        s_snapshot.last_disconnect_reason = 0;
        snprintf(s_snapshot.ip_addr, sizeof(s_snapshot.ip_addr), IPSTR, IP2STR(&event->ip_info.ip));
        system_state_set_wifi_connected(true);
        publish_net_event();
        ESP_LOGI(TAG, "Got IP: %s", s_snapshot.ip_addr);
    }
}

esp_err_t net_manager_init(void)
{
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.state = NET_STATE_DOWN;
    s_snapshot.last_disconnect_reason = 0;
    s_started = false;
    s_wifi_stack_ready = false;
    s_sta_netif = NULL;
    s_disconnect_count = 0;
    s_retry_timer = xTimerCreate("net_retry", pdMS_TO_TICKS(NET_FAST_RETRY_MS), pdFALSE, NULL, retry_timer_cb);
    ESP_RETURN_ON_FALSE(s_retry_timer != NULL, ESP_ERR_NO_MEM, TAG, "failed to create retry timer");
    ESP_RETURN_ON_ERROR(load_credentials_from_nvs(), TAG, "credential load failed");
    return ESP_OK;
}

esp_err_t net_manager_start(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t wifi_config = {0};
    esp_err_t err;

    if (s_started || !s_snapshot.has_credentials) {
        if (!s_snapshot.has_credentials) {
            ESP_LOGW(TAG, "Wi-Fi credentials not configured; net_manager staying down");
        }
        s_started = true;
        publish_net_event();
        return ESP_OK;
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(s_sta_netif != NULL, ESP_ERR_NO_MEM, TAG, "failed to create wifi sta netif");

    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT,
                                                            ESP_EVENT_ANY_ID,
                                                            &wifi_event_handler,
                                                            NULL,
                                                            &s_wifi_event_instance),
                        TAG,
                        "failed to register wifi event handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT,
                                                            IP_EVENT_STA_GOT_IP,
                                                            &wifi_event_handler,
                                                            NULL,
                                                            &s_ip_event_instance),
                        TAG,
                        "failed to register ip event handler");

    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "failed to set wifi storage");
    memcpy(wifi_config.sta.ssid,
           s_snapshot.ssid,
           bounded_cstr_len(s_snapshot.ssid, sizeof(wifi_config.sta.ssid)));
    memcpy(wifi_config.sta.password,
           s_wifi_password,
           bounded_cstr_len(s_wifi_password, sizeof(wifi_config.sta.password)));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "failed to set wifi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "failed to set wifi config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "failed to start wifi");

    s_snapshot.state = NET_STATE_CONNECTING;
    s_started = true;
    s_wifi_stack_ready = true;
    publish_net_event();
    return ESP_OK;
}

const net_snapshot_t *net_manager_get_snapshot(void)
{
    return &s_snapshot;
}

bool net_manager_is_connected(void)
{
    return s_snapshot.wifi_connected && s_snapshot.ip_ready;
}

bool net_manager_has_credentials(void)
{
    return s_snapshot.has_credentials;
}

void net_manager_set_connected_for_test(bool connected)
{
    s_snapshot.wifi_connected = connected;
    s_snapshot.ip_ready = connected;
    s_snapshot.auth_failed = false;
    s_snapshot.has_credentials = true;
    s_snapshot.state = connected ? NET_STATE_UP : NET_STATE_DOWN;
    if (connected) {
        snprintf(s_snapshot.ssid, sizeof(s_snapshot.ssid), "%s", "stub-network");
        snprintf(s_snapshot.ip_addr, sizeof(s_snapshot.ip_addr), "%s", "192.168.1.2");
    } else {
        s_snapshot.ssid[0] = '\0';
        s_snapshot.ip_addr[0] = '\0';
    }
}
