#ifndef SERVICE_SETTINGS_H
#define SERVICE_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "net_manager.h"

#define SETTINGS_SERVICE_STATUS_MAX 64
#define SETTINGS_SERVICE_SCAN_MAX 16

typedef enum {
    SETTINGS_SERVICE_OP_IDLE = 0,
    SETTINGS_SERVICE_OP_SCANNING,
    SETTINGS_SERVICE_OP_SAVING,
    SETTINGS_SERVICE_OP_REMOVING,
} settings_service_operation_t;

typedef struct {
    bool busy;
    bool last_op_failed;
    settings_service_operation_t operation;
    char status_text[SETTINGS_SERVICE_STATUS_MAX];
    net_snapshot_t net;
    net_profile_summary_t profiles[NET_PROFILE_MAX];
    size_t profile_count;
    net_scan_ap_t scan_results[SETTINGS_SERVICE_SCAN_MAX];
    size_t scan_count;
} settings_snapshot_t;

esp_err_t settings_service_init(void);
void settings_service_get_snapshot(settings_snapshot_t *out);
esp_err_t settings_service_request_scan(void);
esp_err_t settings_service_request_add_or_update(const char *ssid, const char *password,
                                                 bool hidden);
esp_err_t settings_service_request_remove(const char *ssid);

#endif
