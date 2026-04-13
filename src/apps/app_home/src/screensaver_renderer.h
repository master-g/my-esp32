/*
 * Home screensaver background renderer.
 *
 * CPU-friendly port of docs/plan/screensaver.glsl, shared by the direct-mode
 * path and the LVGL fallback path.
 */

#ifndef SCREENSAVER_RENDERER_H
#define SCREENSAVER_RENDERER_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

bool screensaver_renderer_init(uint16_t width, uint16_t height);
void screensaver_renderer_render(lv_color32_t *pixels, uint32_t stride_px, uint32_t time_ms);
void screensaver_renderer_get_dimensions(uint16_t *width, uint16_t *height);
void screensaver_renderer_deinit(void);

#endif /* SCREENSAVER_RENDERER_H */
