#ifndef NET_MANAGER_H
#define NET_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

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
    char ssid[33];
    char ip_addr[16];
} net_snapshot_t;

esp_err_t net_manager_init(void);
esp_err_t net_manager_start(void);
const net_snapshot_t *net_manager_get_snapshot(void);
bool net_manager_is_connected(void);
bool net_manager_has_credentials(void);
void net_manager_set_connected_for_test(bool connected);

#endif
