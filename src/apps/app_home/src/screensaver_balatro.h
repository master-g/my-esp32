/*
 * Balatro-inspired screensaver renderer.
 *
 * CPU-friendly approximation of the Balatro background shader using
 * precomputed polar tables and LUT-based trig. Designed for the
 * 160x43 low-res canvas that LVGL scales 4x to fill 640x172.
 *
 * Original shader: docs/plan/balatro-original.glsl
 * Design doc:      docs/plan/balatro-screensaver-cpu-plan.md
 */

#ifndef SCREENSAVER_BALATRO_H
#define SCREENSAVER_BALATRO_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

/* Allocate and populate the precomputed polar table.
 * Call once from create_screensaver_overlay().
 * Returns false if allocation fails (caller should fall back). */
bool balatro_init(uint16_t width, uint16_t height);

/* Render one frame into the canvas pixel buffer.
 *   pixels     - ARGB8888 row-major buffer
 *   stride_px  - row stride in pixels (may differ from width due to alignment)
 *   phase_deg  - primary animation phase, 0-359 (controls flow speed)
 */
void balatro_render(lv_color32_t *pixels, uint32_t stride_px, uint16_t phase_deg);

/* Free the precomputed polar table. */
void balatro_deinit(void);

#endif /* SCREENSAVER_BALATRO_H */
