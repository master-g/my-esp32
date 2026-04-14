#include "service_time.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bsp_rtc.h"
#include "core_types/app_event.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "net_manager.h"
#include "nvs.h"

static const char *TAG = "time_service";
static const char *s_ntp_servers[] = {
    "ntp.aliyun.com",
    "cn.pool.ntp.org",
    "pool.ntp.org",
};

static time_snapshot_t s_snapshot;
static SemaphoreHandle_t s_mutex;
static QueueHandle_t s_command_queue;
static bool s_rtc_valid;
static bool s_ntp_synced;
static bool s_started;
static bool s_sync_in_progress;
static char s_timezone[TIME_SERVICE_TIMEZONE_TZ_MAX];
static char s_timezone_name[TIME_SERVICE_TIMEZONE_NAME_MAX];

typedef enum {
    TIME_CMD_SYNC_NTP = 1,
} time_service_cmd_t;

static void set_placeholder(void)
{
    s_snapshot.now_epoch_s = 0;
    snprintf(s_snapshot.time_text, sizeof(s_snapshot.time_text), "%s", "--:--:--");
    snprintf(s_snapshot.date_text, sizeof(s_snapshot.date_text), "%s", "Time not synced");
    snprintf(s_snapshot.weekday_text, sizeof(s_snapshot.weekday_text), "%s", "--");
}

static void apply_timezone(const char *tz)
{
    if (tz == NULL || tz[0] == '\0') {
        return;
    }

    setenv("TZ", tz, 1);
    tzset();
}

static esp_err_t load_timezone_from_nvs(void)
{
    nvs_handle_t handle = 0;
    size_t required = sizeof(s_timezone);
    size_t name_required = sizeof(s_timezone_name);
    esp_err_t err = nvs_open("time", NVS_READWRITE, &handle);

    ESP_RETURN_ON_ERROR(err, TAG, "failed to open time namespace");

    memset(s_timezone, 0, sizeof(s_timezone));
    memset(s_timezone_name, 0, sizeof(s_timezone_name));
    err = nvs_get_str(handle, "timezone", s_timezone, &required);
    if (err == ESP_ERR_NVS_NOT_FOUND || s_timezone[0] == '\0') {
        snprintf(s_timezone, sizeof(s_timezone), "%s", CONFIG_DASH_TIMEZONE_TZ);
        ESP_RETURN_ON_ERROR(nvs_set_str(handle, "timezone", s_timezone), TAG,
                            "failed to seed timezone");
        err = ESP_OK;
    }

    if (err == ESP_OK) {
        esp_err_t name_err = nvs_get_str(handle, "timezone_name", s_timezone_name, &name_required);
        if (name_err == ESP_ERR_NVS_NOT_FOUND && strcmp(s_timezone, CONFIG_DASH_TIMEZONE_TZ) == 0) {
            snprintf(s_timezone_name, sizeof(s_timezone_name), "%s", CONFIG_DASH_TIMEZONE_NAME);
            ESP_RETURN_ON_ERROR(nvs_set_str(handle, "timezone_name", s_timezone_name), TAG,
                                "failed to seed timezone name");
        } else if (name_err == ESP_OK) {
            (void)name_err;
        } else if (name_err != ESP_ERR_NVS_NOT_FOUND) {
            nvs_close(handle);
            return name_err;
        }
    }

    ESP_RETURN_ON_ERROR(nvs_commit(handle), TAG, "failed to commit timezone settings");
    nvs_close(handle);

    ESP_RETURN_ON_ERROR(err, TAG, "failed to load timezone");
    apply_timezone(s_timezone);
    return ESP_OK;
}

static void update_text_from_epoch(uint32_t epoch_s)
{
    struct tm tm_value;
    time_t now = (time_t)epoch_s;

    memset(&tm_value, 0, sizeof(tm_value));
    localtime_r(&now, &tm_value);

    s_snapshot.now_epoch_s = epoch_s;
    strftime(s_snapshot.time_text, sizeof(s_snapshot.time_text), "%H:%M:%S", &tm_value);
    strftime(s_snapshot.date_text, sizeof(s_snapshot.date_text), "%Y-%m-%d", &tm_value);
    strftime(s_snapshot.weekday_text, sizeof(s_snapshot.weekday_text), "%a", &tm_value);
}

static bool time_service_is_valid_locked(void) { return s_rtc_valid || s_ntp_synced; }

static void time_service_refresh_now_locked(void)
{
    time_t now = time(NULL);

    s_snapshot.rtc_valid = s_rtc_valid;
    s_snapshot.ntp_synced = s_ntp_synced;

    if (time_service_is_valid_locked() && now > 1700000000) {
        update_text_from_epoch((uint32_t)now);
    } else {
        set_placeholder();
    }
}

static void time_service_get_sync_flags(bool *rtc_valid, bool *ntp_synced, bool *sync_in_progress)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (rtc_valid != NULL) {
        *rtc_valid = s_rtc_valid;
    }
    if (ntp_synced != NULL) {
        *ntp_synced = s_ntp_synced;
    }
    if (sync_in_progress != NULL) {
        *sync_in_progress = s_sync_in_progress;
    }
    xSemaphoreGive(s_mutex);
}

void time_service_refresh_now(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    time_service_refresh_now_locked();
    xSemaphoreGive(s_mutex);
}

void time_service_get_current_text(char *time_text, size_t time_text_size, uint32_t *epoch_s_out)
{
    time_t now = time(NULL);
    struct tm tm_value;

    if (epoch_s_out != NULL) {
        *epoch_s_out = 0;
    }

    if (time_text == NULL || time_text_size == 0) {
        return;
    }

    if (!time_service_is_valid() || now <= 1700000000) {
        snprintf(time_text, time_text_size, "%s", "--:--:--");
        return;
    }

    memset(&tm_value, 0, sizeof(tm_value));
    localtime_r(&now, &tm_value);
    strftime(time_text, time_text_size, "%H:%M:%S", &tm_value);
    if (epoch_s_out != NULL) {
        *epoch_s_out = (uint32_t)now;
    }
}

static esp_err_t restore_from_rtc(void)
{
    uint32_t epoch_s = 0;
    struct timeval tv = {0};
    char time_text[sizeof(s_snapshot.time_text)] = {0};
    esp_err_t err = bsp_rtc_read_epoch(&epoch_s);

    if (err != ESP_OK) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_rtc_valid = false;
        time_service_refresh_now_locked();
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "RTC restore unavailable: %s", esp_err_to_name(err));
        return ESP_OK;
    }

    tv.tv_sec = epoch_s;
    settimeofday(&tv, NULL);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_rtc_valid = true;
    time_service_refresh_now_locked();
    snprintf(time_text, sizeof(time_text), "%s", s_snapshot.time_text);
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "RTC restore complete: epoch=%" PRIu32 " time=%s", epoch_s, time_text);
    return ESP_OK;
}

static esp_err_t do_sntp_sync(void)
{
    esp_sntp_config_t config = {
        .smooth_sync = false,
        .server_from_dhcp = false,
        .wait_for_sync = true,
        .start = true,
        .sync_cb = NULL,
        .renew_servers_after_new_IP = false,
        .ip_event_to_renew = IP_EVENT_STA_GOT_IP,
        .index_of_first_server = 0,
        .num_of_servers = 3,
        .servers = {"ntp.aliyun.com", "cn.pool.ntp.org", "pool.ntp.org"},
    };
    time_t now = 0;
    esp_err_t err;
    int retry = 0;
    const int retry_count = 15;

    err = esp_netif_sntp_init(&config);
    if (err == ESP_ERR_INVALID_STATE) {
        esp_netif_sntp_deinit();
        err = esp_netif_sntp_init(&config);
    }
    ESP_RETURN_ON_ERROR(err, TAG, "failed to init sntp");

    while ((err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(2000))) == ESP_ERR_TIMEOUT &&
           ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for SNTP sync... (%d/%d)", retry, retry_count);
    }

    if (err != ESP_OK) {
        esp_netif_sntp_deinit();
        return err;
    }

    time(&now);
    if (now <= 1700000000) {
        esp_netif_sntp_deinit();
        return ESP_ERR_INVALID_RESPONSE;
    }

    {
        bool rtc_write_ok = bsp_rtc_write_epoch((uint32_t)now) == ESP_OK;

        if (rtc_write_ok) {
            ESP_LOGI(TAG, "RTC writeback complete: epoch=%" PRIu32, (uint32_t)now);
        }
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_ntp_synced = true;
        if (rtc_write_ok) {
            s_rtc_valid = true;
        }
        time_service_refresh_now_locked();
        xSemaphoreGive(s_mutex);
    }
    esp_netif_sntp_deinit();
    ESP_LOGI(TAG, "SNTP sync complete using %s", s_ntp_servers[0]);
    return ESP_OK;
}

static void time_service_task(void *arg)
{
    time_service_cmd_t cmd;
    (void)arg;

    for (;;) {
        if (xQueueReceive(s_command_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (cmd != TIME_CMD_SYNC_NTP) {
            continue;
        }
        if (!net_manager_is_connected()) {
            continue;
        }
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (s_sync_in_progress) {
            xSemaphoreGive(s_mutex);
            continue;
        }
        s_sync_in_progress = true;
        xSemaphoreGive(s_mutex);
        do_sntp_sync();
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_sync_in_progress = false;
        xSemaphoreGive(s_mutex);
    }
}

static void time_service_event_handler(const app_event_t *event, void *context)
{
    (void)context;
    if (event == NULL || !s_started) {
        return;
    }

    {
        bool ntp_synced = false;
        bool sync_in_progress = false;

        time_service_get_sync_flags(NULL, &ntp_synced, &sync_in_progress);
        if (event->type == APP_EVENT_NET_CHANGED && net_manager_is_connected() && !ntp_synced &&
            !sync_in_progress) {
            time_service_sync_ntp();
        }
    }
}

esp_err_t time_service_init(void)
{
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_mutex != NULL, ESP_ERR_NO_MEM, TAG, "time mutex alloc failed");
    s_command_queue = NULL;
    s_rtc_valid = false;
    s_ntp_synced = false;
    s_started = false;
    s_sync_in_progress = false;

    set_placeholder();
    ESP_RETURN_ON_ERROR(load_timezone_from_nvs(), TAG, "failed to load timezone");
    ESP_RETURN_ON_ERROR(bsp_rtc_init(), TAG, "rtc init failed");
    ESP_RETURN_ON_ERROR(restore_from_rtc(), TAG, "rtc restore failed");
    ESP_RETURN_ON_ERROR(event_bus_subscribe(time_service_event_handler, NULL), TAG,
                        "failed to subscribe to event bus");
    time_service_refresh_now();
    return ESP_OK;
}

esp_err_t time_service_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    s_command_queue = xQueueCreate(4, sizeof(time_service_cmd_t));
    ESP_RETURN_ON_FALSE(s_command_queue != NULL, ESP_ERR_NO_MEM, TAG,
                        "failed to create time queue");
    {
        BaseType_t ret =
            xTaskCreatePinnedToCore(time_service_task, "time_service", 4096, NULL, 4, NULL, 1);
        ESP_RETURN_ON_FALSE(ret == pdPASS, ESP_ERR_NO_MEM, TAG, "time task create failed");
    }
    s_started = true;

    {
        bool ntp_synced = false;

        time_service_get_sync_flags(NULL, &ntp_synced, NULL);
        if (net_manager_is_connected() && !ntp_synced) {
            time_service_sync_ntp();
        }
    }
    return ESP_OK;
}

esp_err_t time_service_sync_ntp(void)
{
    time_service_cmd_t cmd = TIME_CMD_SYNC_NTP;

    ESP_RETURN_ON_FALSE(s_started, ESP_ERR_INVALID_STATE, TAG, "time service not started");
    ESP_RETURN_ON_FALSE(net_manager_is_connected(), ESP_ERR_INVALID_STATE, TAG,
                        "network not connected");
    {
        bool sync_in_progress = false;

        time_service_get_sync_flags(NULL, NULL, &sync_in_progress);
        if (sync_in_progress) {
            return ESP_OK;
        }
    }

    return (xQueueSend(s_command_queue, &cmd, 0) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

void time_service_get_snapshot(time_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    time_service_refresh_now_locked();
    *out = s_snapshot;
    xSemaphoreGive(s_mutex);
}

void time_service_get_timezone_name(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    snprintf(out, out_size, "%s", s_timezone_name);
    xSemaphoreGive(s_mutex);
}

void time_service_get_timezone_tz(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    snprintf(out, out_size, "%s", s_timezone);
    xSemaphoreGive(s_mutex);
}

esp_err_t time_service_apply_timezone_config(const char *timezone_name, const char *timezone_tz)
{
    if (timezone_tz == NULL || timezone_tz[0] == '\0' ||
        strlen(timezone_tz) >= sizeof(s_timezone)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (timezone_name == NULL || strlen(timezone_name) >= sizeof(s_timezone_name)) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    snprintf(s_timezone, sizeof(s_timezone), "%s", timezone_tz);
    snprintf(s_timezone_name, sizeof(s_timezone_name), "%s", timezone_name);
    apply_timezone(s_timezone);
    time_service_refresh_now_locked();
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

bool time_service_is_valid(void)
{
    bool valid = false;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    valid = time_service_is_valid_locked();
    xSemaphoreGive(s_mutex);
    return valid;
}
