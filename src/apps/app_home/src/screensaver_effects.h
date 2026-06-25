/*
 * Screensaver background-effect library: interface, static registry, and
 * random per-entry selection. Each effect fills a character grid for one frame
 * through a pixel_writer_t; the fixed centered clock is composited separately
 * by the render pipeline (U3), so effects never touch the clock or lifecycle.
 */

#ifndef SCREENSAVER_EFFECTS_H
#define SCREENSAVER_EFFECTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "screensaver_glyphs.h"

/* Character cell size in logical pixels. Effects place glyphs on this grid:
 * a cell (col,row) maps to logical pixel (col*SS_CELL_W, row*SS_CELL_H), glyph
 * drawn at scale 1 (5x7 within the cell). */
#define SS_CELL_W 8
#define SS_CELL_H 12

typedef struct {
    const char *name;
    size_t ctx_size; /* bytes of per-effect state; 0 for stateless effects */
    void (*reset)(void *ctx, uint16_t cols, uint16_t rows);
    void (*render)(void *ctx, pixel_writer_t writer, void *writer_ctx, uint16_t cols, uint16_t rows,
                   uint32_t time_ms);
} screensaver_effect_t;

/* Allocate the shared per-effect state buffer (sized to the largest effect),
 * preferring PSRAM. Call once before selecting. Returns false on alloc failure
 * (the caller can still run the clock over a blank background). */
bool screensaver_effects_init(void);
void screensaver_effects_deinit(void);

int screensaver_effects_count(void);

/* Pick a random effect, avoiding the immediately previous one (relaxed when
 * only one effect exists), reset its state for the grid, and make it current.
 * Returns the chosen index, or -1 when the registry is empty. */
int screensaver_effects_select(uint16_t cols, uint16_t rows);

/* Select the effect whose name matches (case-sensitive), reset its state for
 * the grid, and make it current. Returns the chosen index, or -1 when name is
 * NULL/empty or unknown (caller falls back to random selection). */
int screensaver_effects_select_named(const char *name, uint16_t cols, uint16_t rows);

/* Render the current effect's background for one frame. No-op if no effect is
 * selected (caller renders the clock over a blank background). */
void screensaver_effects_render(pixel_writer_t writer, void *writer_ctx, uint16_t cols,
                                uint16_t rows, uint32_t time_ms);

const char *screensaver_effects_current_name(void);

#endif /* SCREENSAVER_EFFECTS_H */
