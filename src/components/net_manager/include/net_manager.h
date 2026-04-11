#ifndef NET_MANAGER_H
#define NET_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define NET_MANAGER_SSID_MAX 33
#define NET_MANAGER_PASSWORD_MAX 65
#define NET_PROFILE_MAX 5

typedef enum {
    NET_STATE_DOWN = 0,
    NET_STATE_CONNECTING,
    NET_STATE_UP,
} net_state_t;

typedef struct {
    net_state_t state;
    bool wifi_connected;
    bool ip_ready;
    bool auth_failed;
    bool has_credentials;
    int32_t last_disconnect_reason;
    char ssid[NET_MANAGER_SSID_MAX];
    char ip_addr[16];
} net_snapshot_t;

typedef struct {
    char ssid[NET_MANAGER_SSID_MAX];
    bool has_password;
    bool hidden;
    bool active;
} net_profile_summary_t;

typedef struct {
    bool configured;
    char ssid[NET_MANAGER_SSID_MAX];
    bool hidden;
} net_active_profile_t;

typedef struct {
    char ssid[NET_MANAGER_SSID_MAX];
    int8_t rssi;
    uint8_t auth_mode;
} net_scan_ap_t;

esp_err_t net_manager_init(void);
esp_err_t net_manager_start(void);
void net_manager_get_snapshot(net_snapshot_t *out);
bool net_manager_is_connected(void);
bool net_manager_has_credentials(void);
void net_manager_list_profiles(net_profile_summary_t *out, size_t max, size_t *out_count);
void net_manager_get_active_profile(net_active_profile_t *out);
esp_err_t net_manager_add_or_update_profile(const char *ssid, const char *password, bool hidden);
esp_err_t net_manager_remove_profile(const char *ssid);
esp_err_t net_manager_scan_access_points(net_scan_ap_t *results, size_t max_results,
                                         size_t *out_count);
bool net_manager_scan_ap_auth_required(const net_scan_ap_t *result);
void net_manager_set_connected_for_test(bool connected);

#endif
