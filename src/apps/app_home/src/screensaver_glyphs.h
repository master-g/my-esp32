/*
 * Shared 5x7 bitmap glyph table + blitter for the screensaver.
 *
 * Used by the fixed centered clock and by every background effect. Glyphs are
 * drawn in logical (landscape) coordinates through a pixel_writer_t, so the
 * same call works against the native RGB565 framebuffer (put_pixel_native),
 * the RGB565 snapshot buffer (put_pixel_logical_buffer), and the ARGB8888
 * LVGL fallback canvas (put_pixel_logical_canvas).
 */

#ifndef SCREENSAVER_GLYPHS_H
#define SCREENSAVER_GLYPHS_H

#include <stdbool.h>
#include <stdint.h>

/* One pixel write at logical (x, y) with an RGB565 color. */
typedef void (*pixel_writer_t)(void *ctx, int32_t x, int32_t y, uint16_t color);

/*
 * Box-drawing glyphs for the pipes effect. These are non-printable byte codes
 * (below 0x20) so they never collide with the printable characters other
 * effects emit; the pipes effect passes them as the `ch` argument.
 */
#define SS_GLYPH_PIPE_H 0x01U  /* horizontal --- */
#define SS_GLYPH_PIPE_V 0x02U  /* vertical    |  */
#define SS_GLYPH_PIPE_TL 0x03U /* corner      ,- */
#define SS_GLYPH_PIPE_TR 0x04U /* corner      -, */
#define SS_GLYPH_PIPE_BL 0x05U /* corner      '- */
#define SS_GLYPH_PIPE_BR 0x06U /* corner      -' */

/* Returns the 7 row-bytes (low 5 bits each) for `c`; missing glyphs fall back
 * to the space glyph (all zero) so an effect can never render garbage. */
const uint8_t *ss_glyph_rows(char c);

/* Blit one glyph / a string at logical (x, y), each font pixel scaled to a
 * scale*scale block, only writing lit pixels through `writer`. */
void ss_glyph_draw_char(pixel_writer_t writer, void *ctx, int32_t x, int32_t y, int32_t scale,
                        char ch, uint16_t color);
void ss_glyph_draw_text(pixel_writer_t writer, void *ctx, int32_t x, int32_t y, int32_t scale,
                        const char *text, uint16_t color);

/* On-target self-check (ponytail): known glyph resolves, missing glyph falls
 * back to space. Returns true on pass; logged once at screensaver init. */
bool ss_glyph_selftest(void);

#endif /* SCREENSAVER_GLYPHS_H */
