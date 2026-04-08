#ifndef SERVICE_TIME_H
#define SERVICE_TIME_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool rtc_valid;
    bool ntp_synced;
    uint32_t now_epoch_s;
    char hhmm[6];
    char date_text[24];
    char weekday_text[12];
} time_snapshot_t;

esp_err_t time_service_init(void);
esp_err_t time_service_start(void);
esp_err_t time_service_sync_ntp(void);
void time_service_refresh_now(void);
const time_snapshot_t *time_service_get_snapshot(void);
bool time_service_is_valid(void);

#endif
