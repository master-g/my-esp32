#ifndef APP_HOME_SPRITE_CODEC_H
#define APP_HOME_SPRITE_CODEC_H

#include <stdint.h>

/* Home sprites are stored as indexed, cropped, run-length encoded frames (see
 * tools/sprite_convert.py) and expanded one frame at a time into a 64x64
 * ARGB8888 buffer. No LVGL or ESP-IDF dependency, so `make test-host` can check
 * the decoder against the converter's reference output. */

#define SPRITE_FRAME_W 64
#define SPRITE_FRAME_H 64
#define SPRITE_FRAME_BYTES (SPRITE_FRAME_W * SPRITE_FRAME_H * 4)

typedef struct {
    uint8_t x, y, w, h; /* crop rectangle inside the frame, shared by all frames */
    uint8_t frame_count;
    const uint16_t *palette;      /* RGB565; index 0 is transparent */
    const uint8_t *const *frames; /* per frame: (run length, palette index) pairs covering w*h */
} sprite_sheet_t;

/* Expand one frame into `out` (SPRITE_FRAME_BYTES, byte order B, G, R, A). */
void sprite_decode_frame(const sprite_sheet_t *sheet, uint8_t frame, uint8_t *out);

#endif
