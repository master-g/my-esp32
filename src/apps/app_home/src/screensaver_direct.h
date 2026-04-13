#ifndef SCREENSAVER_DIRECT_H
#define SCREENSAVER_DIRECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint16_t compose_ms_x10;
    uint16_t text_ms_x10;
    uint16_t wait_ms_x10;
    uint16_t push_ms_x10;
} screensaver_direct_perf_snapshot_t;

bool screensaver_direct_init(void);
bool screensaver_direct_is_ready(void);
void screensaver_direct_reset(void);
void screensaver_direct_restore_background(void);
bool screensaver_direct_wait_idle(uint32_t timeout_ms);
void screensaver_direct_deinit(void);
void screensaver_direct_get_perf_snapshot(screensaver_direct_perf_snapshot_t *out);
esp_err_t screensaver_direct_render_and_push(uint32_t time_ms, const char *time_text);
esp_err_t screensaver_direct_render_snapshot_rgb565(uint32_t time_ms, const char *time_text,
                                                    uint16_t *pixels, size_t capacity_bytes,
                                                    uint16_t *out_width, uint16_t *out_height,
                                                    uint16_t *out_stride_bytes);

#endif
