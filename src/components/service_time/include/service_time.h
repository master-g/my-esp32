#ifndef SERVICE_TIME_H
#define SERVICE_TIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define TIME_SERVICE_TIMEZONE_NAME_MAX 64
#define TIME_SERVICE_TIMEZONE_TZ_MAX 64

typedef struct {
    bool rtc_valid;
    bool ntp_synced;
    uint32_t now_epoch_s;
    char time_text[9];
    char date_text[24];
    char weekday_text[12];
} time_snapshot_t;

esp_err_t time_service_init(void);
esp_err_t time_service_start(void);
esp_err_t time_service_sync_ntp(void);
void time_service_refresh_now(void);
void time_service_get_current_text(char *time_text, size_t time_text_size, uint32_t *epoch_s_out);
const char *time_service_get_timezone_name(void);
const char *time_service_get_timezone_tz(void);
esp_err_t time_service_apply_timezone_config(const char *timezone_name, const char *timezone_tz);
void time_service_get_snapshot(time_snapshot_t *out);
bool time_service_is_valid(void);

#endif
