#include "net_manager.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_types/app_event.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_wifi.h"
#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "nvs.h"
#include "system_state.h"

#define NET_NVS_NAMESPACE "wifi"
#define NET_NVS_KEY_COUNT "count"
#define NET_NVS_KEY_LEGACY_SSID "ssid"
#define NET_NVS_KEY_LEGACY_PASSWORD "password"
#define NET_FAST_RETRY_MS 2000
#define NET_SLOW_RETRY_MS 10000
#define NET_FAST_RETRY_COUNT 3
#define NET_SCAN_RESULT_MAX 16

typedef struct {
    char ssid[NET_MANAGER_SSID_MAX];
    char password[NET_MANAGER_PASSWORD_MAX];
    bool hidden;
} wifi_profile_t;

typedef struct {
    uint8_t profile_idx;
    int8_t rssi;
} wifi_candidate_t;

static const char *TAG = "net_manager";
static net_snapshot_t s_snapshot;
static SemaphoreHandle_t s_mutex;
static bool s_started;
static bool s_wifi_stack_ready;
static bool s_wifi_driver_started;
static esp_netif_t *s_sta_netif;
static esp_event_handler_instance_t s_wifi_event_instance;
static esp_event_handler_instance_t s_ip_event_instance;
static TimerHandle_t s_retry_timer;
static uint32_t s_disconnect_count;
static bool s_retry_requires_rescan;
static bool s_ignore_disconnect_event;
static wifi_profile_t s_profiles[NET_PROFILE_MAX];
static uint8_t s_profile_count;
static int8_t s_current_profile_idx = -1;
static wifi_candidate_t s_candidates[NET_PROFILE_MAX];
static uint8_t s_candidate_count;
static uint8_t s_candidate_pos;

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                               void *event_data);

static bool has_text(const char *text) { return text != NULL && text[0] != '\0'; }

static void publish_net_event(void)
{
    app_event_t event = {
        .type = APP_EVENT_NET_CHANGED,
        .payload = NULL,
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

static void copy_text_buffer(char *dst, size_t dst_size, const char *src)
{
    size_t len;

    if (dst == NULL || dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    len = bounded_cstr_len(src, dst_size - 1);
    memcpy(dst, src, len);
    dst[len] = '\0';
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

static void build_profile_key(char *dst, size_t dst_size, const char *prefix, uint8_t index)
{
    snprintf(dst, dst_size, "%s_%" PRIu8, prefix, index);
}

static int find_profile_index_locked(const char *ssid)
{
    uint8_t i;

    if (!has_text(ssid)) {
        return -1;
    }

    for (i = 0; i < s_profile_count; ++i) {
        if (strcmp(s_profiles[i].ssid, ssid) == 0) {
            return (int)i;
        }
    }

    return -1;
}

static void reset_candidate_list_locked(void)
{
    memset(s_candidates, 0, sizeof(s_candidates));
    s_candidate_count = 0;
    s_candidate_pos = 0;
}

static void update_snapshot_credentials_locked(void)
{
    s_snapshot.has_credentials = (s_profile_count > 0);
    if (s_current_profile_idx < 0 || s_current_profile_idx >= s_profile_count) {
        if (s_profile_count == 0) {
            s_snapshot.ssid[0] = '\0';
        }
        return;
    }

    copy_text_buffer(s_snapshot.ssid, sizeof(s_snapshot.ssid),
                     s_profiles[s_current_profile_idx].ssid);
}

static void clear_runtime_connection_locked(bool clear_ssid)
{
    s_snapshot.wifi_connected = false;
    s_snapshot.ip_ready = false;
    s_snapshot.auth_failed = false;
    s_snapshot.last_disconnect_reason = 0;
    s_snapshot.ip_addr[0] = '\0';
    s_snapshot.state = s_snapshot.has_credentials ? NET_STATE_CONNECTING : NET_STATE_DOWN;
    if (clear_ssid) {
        s_snapshot.ssid[0] = '\0';
    }
}

static void copy_profiles_snapshot(wifi_profile_t *profiles, uint8_t *count, int8_t *current_idx)
{
    if (profiles == NULL || count == NULL || current_idx == NULL) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(profiles, s_profiles, sizeof(s_profiles));
    *count = s_profile_count;
    *current_idx = s_current_profile_idx;
    xSemaphoreGive(s_mutex);
}

static esp_err_t read_nvs_string(nvs_handle_t handle, const char *key, char *buffer,
                                 size_t buffer_len)
{
    size_t required = buffer_len;
    esp_err_t err = nvs_get_str(handle, key, buffer, &required);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        buffer[0] = '\0';
        return ESP_OK;
    }

    return err;
}

static void erase_key_if_present(nvs_handle_t handle, const char *key)
{
    esp_err_t err = nvs_erase_key(handle, key);

    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "failed to erase NVS key `%s`: %s", key, esp_err_to_name(err));
    }
}

static esp_err_t persist_profiles_to_nvs(void)
{
    wifi_profile_t profiles[NET_PROFILE_MAX];
    uint8_t count = 0;
    nvs_handle_t handle = 0;
    esp_err_t err;
    esp_err_t ret = ESP_OK;
    uint8_t i;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(profiles, s_profiles, sizeof(profiles));
    count = s_profile_count;
    xSemaphoreGive(s_mutex);

    err = nvs_open(NET_NVS_NAMESPACE, NVS_READWRITE, &handle);
    ESP_RETURN_ON_ERROR(err, TAG, "failed to open wifi namespace");

    ESP_GOTO_ON_ERROR(nvs_set_u8(handle, NET_NVS_KEY_COUNT, count), fail, TAG,
                      "failed to persist profile count");

    for (i = 0; i < count; ++i) {
        char key[16];

        build_profile_key(key, sizeof(key), "ssid", i);
        ESP_GOTO_ON_ERROR(nvs_set_str(handle, key, profiles[i].ssid), fail, TAG,
                          "failed to persist %s", key);
        build_profile_key(key, sizeof(key), "pass", i);
        ESP_GOTO_ON_ERROR(nvs_set_str(handle, key, profiles[i].password), fail, TAG,
                          "failed to persist %s", key);
        build_profile_key(key, sizeof(key), "hidden", i);
        ESP_GOTO_ON_ERROR(nvs_set_u8(handle, key, profiles[i].hidden ? 1U : 0U), fail, TAG,
                          "failed to persist %s", key);
    }

    for (; i < NET_PROFILE_MAX; ++i) {
        char key[16];

        build_profile_key(key, sizeof(key), "ssid", i);
        erase_key_if_present(handle, key);
        build_profile_key(key, sizeof(key), "pass", i);
        erase_key_if_present(handle, key);
        build_profile_key(key, sizeof(key), "hidden", i);
        erase_key_if_present(handle, key);
    }

    erase_key_if_present(handle, NET_NVS_KEY_LEGACY_SSID);
    erase_key_if_present(handle, NET_NVS_KEY_LEGACY_PASSWORD);
    ESP_GOTO_ON_ERROR(nvs_commit(handle), fail, TAG, "failed to commit wifi profiles");
    nvs_close(handle);
    return ESP_OK;

fail:
    nvs_close(handle);
    return ret;
}

static esp_err_t load_profiles_from_nvs(void)
{
    wifi_profile_t loaded_profiles[NET_PROFILE_MAX];
    uint8_t loaded_count = 0;
    uint8_t stored_count = 0;
    nvs_handle_t handle = 0;
    esp_err_t err;
    bool migrated_legacy = false;
    uint8_t i;

    memset(loaded_profiles, 0, sizeof(loaded_profiles));
    err = nvs_open(NET_NVS_NAMESPACE, NVS_READWRITE, &handle);
    ESP_RETURN_ON_ERROR(err, TAG, "failed to open wifi namespace");

    err = nvs_get_u8(handle, NET_NVS_KEY_COUNT, &stored_count);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        char legacy_ssid[NET_MANAGER_SSID_MAX] = {0};
        char legacy_password[NET_MANAGER_PASSWORD_MAX] = {0};

        err = read_nvs_string(handle, NET_NVS_KEY_LEGACY_SSID, legacy_ssid, sizeof(legacy_ssid));
        if (err == ESP_OK) {
            err = read_nvs_string(handle, NET_NVS_KEY_LEGACY_PASSWORD, legacy_password,
                                  sizeof(legacy_password));
        }
        if (err == ESP_OK && has_text(legacy_ssid)) {
            copy_text_buffer(loaded_profiles[0].ssid, sizeof(loaded_profiles[0].ssid), legacy_ssid);
            copy_text_buffer(loaded_profiles[0].password, sizeof(loaded_profiles[0].password),
                             legacy_password);
            loaded_profiles[0].hidden = false;
            loaded_count = 1;
            migrated_legacy = true;
            ESP_LOGI(TAG, "Migrated legacy single Wi-Fi credential into profile storage");
        }
        err = ESP_OK;
    } else if (err == ESP_OK) {
        if (stored_count > NET_PROFILE_MAX) {
            stored_count = NET_PROFILE_MAX;
        }

        for (i = 0; i < stored_count; ++i) {
            char key[16];
            uint8_t hidden_value = 0;

            build_profile_key(key, sizeof(key), "ssid", i);
            err = read_nvs_string(handle, key, loaded_profiles[loaded_count].ssid,
                                  sizeof(loaded_profiles[loaded_count].ssid));
            if (err != ESP_OK) {
                break;
            }
            if (!has_text(loaded_profiles[loaded_count].ssid)) {
                continue;
            }

            build_profile_key(key, sizeof(key), "pass", i);
            err = read_nvs_string(handle, key, loaded_profiles[loaded_count].password,
                                  sizeof(loaded_profiles[loaded_count].password));
            if (err != ESP_OK) {
                break;
            }

            build_profile_key(key, sizeof(key), "hidden", i);
            if (nvs_get_u8(handle, key, &hidden_value) == ESP_OK) {
                loaded_profiles[loaded_count].hidden = (hidden_value != 0);
            } else {
                loaded_profiles[loaded_count].hidden = false;
            }
            loaded_count++;
        }
    }

    nvs_close(handle);
    ESP_RETURN_ON_ERROR(err, TAG, "failed loading Wi-Fi profiles");

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(s_profiles, 0, sizeof(s_profiles));
    memcpy(s_profiles, loaded_profiles, sizeof(loaded_profiles));
    s_profile_count = loaded_count;
    s_current_profile_idx = -1;
    reset_candidate_list_locked();
    s_snapshot.has_credentials = (s_profile_count > 0);
    if (s_profile_count == 0) {
        s_snapshot.ssid[0] = '\0';
    }
    xSemaphoreGive(s_mutex);

    if (migrated_legacy) {
        ESP_RETURN_ON_ERROR(persist_profiles_to_nvs(), TAG, "failed to persist migrated profiles");
    }

    return ESP_OK;
}

static esp_err_t ensure_wifi_stack_ready(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err;

    if (s_wifi_stack_ready) {
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

    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        ESP_RETURN_ON_FALSE(s_sta_netif != NULL, ESP_ERR_NO_MEM, TAG,
                            "failed to create wifi sta netif");
    }

    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            &wifi_event_handler, NULL,
                                                            &s_wifi_event_instance),
                        TAG, "failed to register wifi event handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            &wifi_event_handler, NULL,
                                                            &s_ip_event_instance),
                        TAG, "failed to register ip event handler");

    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "failed to set wifi storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "failed to set wifi mode");

    s_wifi_stack_ready = true;
    return ESP_OK;
}

static esp_err_t apply_wifi_config_for_profile(const wifi_profile_t *profile)
{
    wifi_config_t wifi_config = {0};

    if (!s_wifi_stack_ready || profile == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(wifi_config.sta.ssid, profile->ssid,
           bounded_cstr_len(profile->ssid, sizeof(wifi_config.sta.ssid)));
    memcpy(wifi_config.sta.password, profile->password,
           bounded_cstr_len(profile->password, sizeof(wifi_config.sta.password)));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    return esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
}

static esp_err_t scan_access_points_internal(net_scan_ap_t *results, size_t max_results,
                                             size_t *out_count)
{
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 60,
        .scan_time.active.max = 120,
    };
    uint16_t ap_count = 0;
    wifi_ap_record_t *records = NULL;
    size_t i;

    if (results == NULL || out_count == NULL || max_results == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_count = 0;
    ESP_RETURN_ON_ERROR(ensure_wifi_stack_ready(), TAG, "failed to initialize Wi-Fi stack");
    if (!s_wifi_driver_started) {
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "failed to start wifi for scan");
        s_wifi_driver_started = true;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_scan_start(&scan_config, true), TAG, "wifi scan start failed");
    ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_num(&ap_count), TAG, "wifi scan count failed");
    if (ap_count == 0) {
        return ESP_OK;
    }

    records = calloc(ap_count, sizeof(wifi_ap_record_t));
    ESP_RETURN_ON_FALSE(records != NULL, ESP_ERR_NO_MEM, TAG, "wifi scan alloc failed");
    if (esp_wifi_scan_get_ap_records(&ap_count, records) != ESP_OK) {
        free(records);
        return ESP_FAIL;
    }

    for (i = 0; i < ap_count && *out_count < max_results; ++i) {
        if (records[i].ssid[0] == '\0') {
            continue;
        }

        memset(&results[*out_count], 0, sizeof(results[*out_count]));
        copy_text_buffer(results[*out_count].ssid, sizeof(results[*out_count].ssid),
                         (const char *)records[i].ssid);
        results[*out_count].rssi = records[i].rssi;
        results[*out_count].auth_mode = (uint8_t)records[i].authmode;
        (*out_count)++;
    }

    free(records);
    return ESP_OK;
}

static esp_err_t rebuild_candidate_list(void)
{
    wifi_profile_t profiles[NET_PROFILE_MAX];
    wifi_candidate_t candidates[NET_PROFILE_MAX];
    net_scan_ap_t scan_results[NET_SCAN_RESULT_MAX];
    bool included[NET_PROFILE_MAX] = {0};
    bool matched[NET_PROFILE_MAX] = {0};
    int8_t matched_rssi[NET_PROFILE_MAX] = {0};
    size_t scan_count = 0;
    uint8_t local_count = 0;
    uint8_t profile_count = 0;
    int8_t current_idx = -1;
    uint8_t i;

    copy_profiles_snapshot(profiles, &profile_count, &current_idx);
    if (profile_count == 0) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        reset_candidate_list_locked();
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    if (scan_access_points_internal(scan_results, NET_SCAN_RESULT_MAX, &scan_count) != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi scan unavailable while rebuilding candidates; using stored order");
        scan_count = 0;
    }

    for (i = 0; i < profile_count; ++i) {
        size_t j;

        for (j = 0; j < scan_count; ++j) {
            if (strcmp(profiles[i].ssid, scan_results[j].ssid) != 0) {
                continue;
            }

            if (!matched[i] || scan_results[j].rssi > matched_rssi[i]) {
                matched[i] = true;
                matched_rssi[i] = scan_results[j].rssi;
            }
        }
    }

    for (;;) {
        int best = -1;

        for (i = 0; i < profile_count; ++i) {
            if (!matched[i] || included[i]) {
                continue;
            }
            if (best < 0 || matched_rssi[i] > matched_rssi[best]) {
                best = (int)i;
            }
        }

        if (best < 0) {
            break;
        }

        candidates[local_count].profile_idx = (uint8_t)best;
        candidates[local_count].rssi = matched_rssi[best];
        included[best] = true;
        local_count++;
    }

    for (i = 0; i < profile_count; ++i) {
        if (!included[i] && profiles[i].hidden) {
            candidates[local_count].profile_idx = i;
            candidates[local_count].rssi = INT8_MIN;
            included[i] = true;
            local_count++;
        }
    }

    for (i = 0; i < profile_count; ++i) {
        if (!included[i]) {
            candidates[local_count].profile_idx = i;
            candidates[local_count].rssi = INT8_MIN;
            included[i] = true;
            local_count++;
        }
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(s_candidates, 0, sizeof(s_candidates));
    memcpy(s_candidates, candidates, sizeof(candidates));
    s_candidate_count = local_count;
    s_candidate_pos = 0;
    xSemaphoreGive(s_mutex);
    return (local_count > 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t start_connection_to_profile(uint8_t profile_idx)
{
    wifi_profile_t profile = {0};

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (profile_idx >= s_profile_count) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    profile = s_profiles[profile_idx];
    s_current_profile_idx = (int8_t)profile_idx;
    s_disconnect_count = 0;
    s_retry_requires_rescan = false;
    s_snapshot.has_credentials = (s_profile_count > 0);
    s_snapshot.state = NET_STATE_CONNECTING;
    s_snapshot.wifi_connected = false;
    s_snapshot.ip_ready = false;
    s_snapshot.auth_failed = false;
    s_snapshot.last_disconnect_reason = 0;
    s_snapshot.ip_addr[0] = '\0';
    copy_text_buffer(s_snapshot.ssid, sizeof(s_snapshot.ssid), profile.ssid);
    xSemaphoreGive(s_mutex);

    system_state_set_wifi_connected(false);
    if (s_retry_timer != NULL) {
        xTimerStop(s_retry_timer, 0);
    }

    ESP_RETURN_ON_ERROR(ensure_wifi_stack_ready(), TAG, "failed to initialize Wi-Fi stack");
    ESP_RETURN_ON_ERROR(apply_wifi_config_for_profile(&profile), TAG,
                        "failed to apply Wi-Fi profile");

    if (!s_wifi_driver_started) {
        s_wifi_driver_started = true;
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "failed to start wifi");
        publish_net_event();
        return ESP_OK;
    }

    s_ignore_disconnect_event = true;
    (void)esp_wifi_disconnect();
    publish_net_event();
    return esp_wifi_connect();
}

static esp_err_t advance_to_next_candidate(void)
{
    for (;;) {
        uint8_t next_profile = 0;
        bool have_candidate = false;
        esp_err_t err;

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (s_candidate_pos < s_candidate_count) {
            next_profile = s_candidates[s_candidate_pos++].profile_idx;
            have_candidate = true;
        }
        xSemaphoreGive(s_mutex);

        if (!have_candidate) {
            return ESP_ERR_NOT_FOUND;
        }

        err = start_connection_to_profile(next_profile);
        if (err == ESP_OK) {
            return ESP_OK;
        }

        ESP_LOGW(TAG, "failed to start Wi-Fi profile `%u`: %s", (unsigned)next_profile,
                 esp_err_to_name(err));
    }
}

static void schedule_retry(bool rescan)
{
    TickType_t delay_ticks;

    if (s_retry_timer == NULL) {
        return;
    }

    s_retry_requires_rescan = rescan;
    delay_ticks = pdMS_TO_TICKS(rescan ? NET_SLOW_RETRY_MS : NET_FAST_RETRY_MS);
    xTimerStop(s_retry_timer, 0);
    xTimerChangePeriod(s_retry_timer, delay_ticks, 0);
    xTimerStart(s_retry_timer, 0);
}

static esp_err_t reconcile_connection_after_profile_change(bool force_reconnect)
{
    bool keep_current = false;
    int8_t current_idx = -1;
    uint8_t best_idx = UINT8_MAX;
    net_state_t state = NET_STATE_DOWN;
    bool connected = false;

    if (!s_started) {
        return ESP_OK;
    }

    if (rebuild_candidate_list() != ESP_OK) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_current_profile_idx = -1;
        s_snapshot.has_credentials = false;
        clear_runtime_connection_locked(true);
        xSemaphoreGive(s_mutex);
        system_state_set_wifi_connected(false);
        if (s_retry_timer != NULL) {
            xTimerStop(s_retry_timer, 0);
        }
        s_ignore_disconnect_event = true;
        (void)esp_wifi_disconnect();
        publish_net_event();
        return ESP_OK;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    current_idx = s_current_profile_idx;
    if (s_candidate_count > 0) {
        best_idx = s_candidates[0].profile_idx;
    }
    state = s_snapshot.state;
    connected = s_snapshot.wifi_connected && s_snapshot.ip_ready;
    xSemaphoreGive(s_mutex);

    keep_current = !force_reconnect && best_idx != UINT8_MAX && current_idx == (int8_t)best_idx &&
                   (connected || state == NET_STATE_CONNECTING);
    if (keep_current) {
        return ESP_OK;
    }

    return advance_to_next_candidate();
}

static void retry_timer_cb(TimerHandle_t timer)
{
    (void)timer;

    if (!s_started || !net_manager_has_credentials()) {
        return;
    }

    if (s_retry_requires_rescan) {
        /* Do NOT call rebuild_candidate_list() here — it runs a blocking Wi-Fi scan
         * (esp_wifi_scan_start block=true) which would freeze the timer task for ~1.5 s.
         * Instead, reset the candidate position and round-robin the existing stored
         * profiles.  A fresh scan happens the next time the user opens Wi-Fi settings. */
        s_retry_requires_rescan = false;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_candidate_pos = 0;
        xSemaphoreGive(s_mutex);
        if (advance_to_next_candidate() == ESP_OK) {
            return;
        }
        schedule_retry(true);
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_snapshot.state = NET_STATE_CONNECTING;
    xSemaphoreGive(s_mutex);
    publish_net_event();
    (void)esp_wifi_connect();
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                               void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        bool should_connect = false;

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_snapshot.has_credentials = (s_profile_count > 0);
        s_snapshot.state = s_snapshot.has_credentials ? NET_STATE_CONNECTING : NET_STATE_DOWN;
        should_connect = (s_current_profile_idx >= 0 && s_current_profile_idx < s_profile_count);
        xSemaphoreGive(s_mutex);
        publish_net_event();
        if (should_connect) {
            (void)esp_wifi_connect();
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event =
            (const wifi_event_sta_disconnected_t *)event_data;
        bool has_profiles = false;
        bool auth_failure;

        if (s_ignore_disconnect_event) {
            s_ignore_disconnect_event = false;
            return;
        }

        auth_failure = is_auth_failure_reason(event->reason);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        has_profiles = (s_profile_count > 0);
        s_snapshot.state = has_profiles ? NET_STATE_CONNECTING : NET_STATE_DOWN;
        s_snapshot.wifi_connected = false;
        s_snapshot.ip_ready = false;
        s_snapshot.auth_failed = auth_failure;
        s_snapshot.last_disconnect_reason = event->reason;
        s_snapshot.ip_addr[0] = '\0';
        xSemaphoreGive(s_mutex);
        system_state_set_wifi_connected(false);
        publish_net_event();

        if (!has_profiles) {
            return;
        }

        if (auth_failure) {
            if (advance_to_next_candidate() != ESP_OK) {
                s_disconnect_count = NET_FAST_RETRY_COUNT;
                schedule_retry(true);
            }
            ESP_LOGW(TAG, "Wi-Fi auth failed, reason=%" PRIi32, (int32_t)event->reason);
            return;
        }

        if (s_disconnect_count < NET_FAST_RETRY_COUNT) {
            s_disconnect_count++;
            schedule_retry(false);
        } else {
            schedule_retry(true);
        }
        ESP_LOGW(TAG, "Wi-Fi disconnected, reason=%" PRIi32, (int32_t)event->reason);
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;

        s_disconnect_count = 0;
        s_retry_requires_rescan = false;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_snapshot.state = NET_STATE_UP;
        s_snapshot.wifi_connected = true;
        s_snapshot.ip_ready = true;
        s_snapshot.auth_failed = false;
        s_snapshot.last_disconnect_reason = 0;
        snprintf(s_snapshot.ip_addr, sizeof(s_snapshot.ip_addr), IPSTR, IP2STR(&event->ip_info.ip));
        update_snapshot_credentials_locked();
        xSemaphoreGive(s_mutex);
        system_state_set_wifi_connected(true);
        publish_net_event();
        ESP_LOGI(TAG, "Got IP: %s", s_snapshot.ip_addr);
    }
}

esp_err_t net_manager_init(void)
{
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    memset(s_profiles, 0, sizeof(s_profiles));
    s_snapshot.state = NET_STATE_DOWN;
    s_snapshot.last_disconnect_reason = 0;
    s_profile_count = 0;
    s_current_profile_idx = -1;
    s_candidate_count = 0;
    s_candidate_pos = 0;
    s_retry_requires_rescan = false;
    s_ignore_disconnect_event = false;
    s_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_mutex != NULL, ESP_ERR_NO_MEM, TAG, "net mutex alloc failed");
    s_started = false;
    s_wifi_stack_ready = false;
    s_wifi_driver_started = false;
    s_sta_netif = NULL;
    s_disconnect_count = 0;
    s_retry_timer =
        xTimerCreate("net_retry", pdMS_TO_TICKS(NET_FAST_RETRY_MS), pdFALSE, NULL, retry_timer_cb);
    ESP_RETURN_ON_FALSE(s_retry_timer != NULL, ESP_ERR_NO_MEM, TAG, "failed to create retry timer");
    ESP_RETURN_ON_ERROR(load_profiles_from_nvs(), TAG, "profile load failed");
    return ESP_OK;
}

esp_err_t net_manager_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    s_started = true;
    ESP_RETURN_ON_ERROR(ensure_wifi_stack_ready(), TAG, "failed to initialize Wi-Fi stack");

    if (!net_manager_has_credentials()) {
        if (!s_wifi_driver_started) {
            ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "failed to start wifi");
            s_wifi_driver_started = true;
        }

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_snapshot.state = NET_STATE_DOWN;
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "Wi-Fi profiles not configured; Wi-Fi stack is ready for scans");
        publish_net_event();
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(rebuild_candidate_list(), TAG, "failed to build Wi-Fi candidates");
    return advance_to_next_candidate();
}

void net_manager_get_snapshot(net_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_snapshot;
    xSemaphoreGive(s_mutex);
}

bool net_manager_is_connected(void)
{
    bool connected;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    connected = s_snapshot.wifi_connected && s_snapshot.ip_ready;
    xSemaphoreGive(s_mutex);
    return connected;
}

bool net_manager_has_credentials(void)
{
    bool has_credentials;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    has_credentials = (s_profile_count > 0);
    xSemaphoreGive(s_mutex);
    return has_credentials;
}

void net_manager_list_profiles(net_profile_summary_t *out, size_t max, size_t *out_count)
{
    uint8_t i;

    if (out_count == NULL) {
        return;
    }

    *out_count = 0;
    if (out == NULL || max == 0) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (i = 0; i < s_profile_count && *out_count < max; ++i) {
        memset(&out[*out_count], 0, sizeof(out[*out_count]));
        copy_text_buffer(out[*out_count].ssid, sizeof(out[*out_count].ssid), s_profiles[i].ssid);
        out[*out_count].has_password = has_text(s_profiles[i].password);
        out[*out_count].hidden = s_profiles[i].hidden;
        out[*out_count].active = (s_current_profile_idx == (int8_t)i);
        (*out_count)++;
    }
    xSemaphoreGive(s_mutex);
}

void net_manager_get_active_profile(net_active_profile_t *out)
{
    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_current_profile_idx >= 0 && s_current_profile_idx < s_profile_count) {
        out->configured = true;
        out->hidden = s_profiles[s_current_profile_idx].hidden;
        copy_text_buffer(out->ssid, sizeof(out->ssid), s_profiles[s_current_profile_idx].ssid);
    }
    xSemaphoreGive(s_mutex);
}

esp_err_t net_manager_add_or_update_profile(const char *ssid, const char *password, bool hidden)
{
    int profile_idx;
    bool force_reconnect = false;
    bool password_changed = false;

    if (!has_text(ssid) || strlen(ssid) >= NET_MANAGER_SSID_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (password != NULL && strlen(password) >= NET_MANAGER_PASSWORD_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    profile_idx = find_profile_index_locked(ssid);
    if (profile_idx >= 0) {
        if (password != NULL) {
            password_changed = strcmp(s_profiles[profile_idx].password, password) != 0;
            copy_text_buffer(s_profiles[profile_idx].password,
                             sizeof(s_profiles[profile_idx].password), password);
        }
        s_profiles[profile_idx].hidden = hidden;
        force_reconnect = (password_changed && s_current_profile_idx == profile_idx);
    } else {
        if (password == NULL) {
            xSemaphoreGive(s_mutex);
            return ESP_ERR_INVALID_ARG;
        }
        if (s_profile_count >= NET_PROFILE_MAX) {
            xSemaphoreGive(s_mutex);
            return ESP_ERR_NO_MEM;
        }

        copy_text_buffer(s_profiles[s_profile_count].ssid, sizeof(s_profiles[s_profile_count].ssid),
                         ssid);
        copy_text_buffer(s_profiles[s_profile_count].password,
                         sizeof(s_profiles[s_profile_count].password), password);
        s_profiles[s_profile_count].hidden = hidden;
        s_profile_count++;
        s_snapshot.has_credentials = true;
    }
    update_snapshot_credentials_locked();
    xSemaphoreGive(s_mutex);

    ESP_RETURN_ON_ERROR(persist_profiles_to_nvs(), TAG, "failed to persist Wi-Fi profiles");
    return reconcile_connection_after_profile_change(force_reconnect);
}

esp_err_t net_manager_remove_profile(const char *ssid)
{
    int profile_idx;
    bool removed_current = false;

    if (!has_text(ssid)) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    profile_idx = find_profile_index_locked(ssid);
    if (profile_idx < 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    removed_current = (s_current_profile_idx == profile_idx);
    for (uint8_t i = (uint8_t)profile_idx; i + 1 < s_profile_count; ++i) {
        s_profiles[i] = s_profiles[i + 1];
    }
    if (s_profile_count > 0) {
        memset(&s_profiles[s_profile_count - 1], 0, sizeof(s_profiles[s_profile_count - 1]));
        s_profile_count--;
    }

    if (removed_current) {
        s_current_profile_idx = -1;
    } else if (s_current_profile_idx > profile_idx) {
        s_current_profile_idx--;
    }

    s_snapshot.has_credentials = (s_profile_count > 0);
    if (s_profile_count == 0) {
        clear_runtime_connection_locked(true);
        reset_candidate_list_locked();
    } else {
        update_snapshot_credentials_locked();
    }
    xSemaphoreGive(s_mutex);

    ESP_RETURN_ON_ERROR(persist_profiles_to_nvs(), TAG, "failed to persist Wi-Fi profiles");
    return reconcile_connection_after_profile_change(removed_current);
}

esp_err_t net_manager_scan_access_points(net_scan_ap_t *results, size_t max_results,
                                         size_t *out_count)
{
    return scan_access_points_internal(results, max_results, out_count);
}

bool net_manager_scan_ap_auth_required(const net_scan_ap_t *result)
{
    return result != NULL && result->auth_mode != (uint8_t)WIFI_AUTH_OPEN;
}

void net_manager_set_connected_for_test(bool connected)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(s_profiles, 0, sizeof(s_profiles));
    s_profile_count = 1;
    copy_text_buffer(s_profiles[0].ssid, sizeof(s_profiles[0].ssid), "stub-network");
    s_profiles[0].password[0] = '\0';
    s_profiles[0].hidden = false;
    s_current_profile_idx = 0;
    s_snapshot.has_credentials = true;
    s_snapshot.wifi_connected = connected;
    s_snapshot.ip_ready = connected;
    s_snapshot.auth_failed = false;
    s_snapshot.last_disconnect_reason = 0;
    s_snapshot.state = connected ? NET_STATE_UP : NET_STATE_DOWN;
    copy_text_buffer(s_snapshot.ssid, sizeof(s_snapshot.ssid), s_profiles[0].ssid);
    if (connected) {
        snprintf(s_snapshot.ip_addr, sizeof(s_snapshot.ip_addr), "%s", "192.168.1.2");
    } else {
        s_snapshot.ip_addr[0] = '\0';
    }
    xSemaphoreGive(s_mutex);
}
