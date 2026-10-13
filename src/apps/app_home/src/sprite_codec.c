#include "sprite_codec.h"

#include <string.h>

void sprite_decode_frame(const sprite_sheet_t *sheet, uint8_t frame, uint8_t *out)
{
    const uint8_t *in = sheet->frames[frame];
    uint32_t total = (uint32_t)sheet->w * sheet->h;
    uint32_t pos = 0;

    memset(out, 0, SPRITE_FRAME_BYTES);
    while (pos < total) {
        uint8_t run = *in++;
        uint8_t index = *in++;

        if (index != 0) {
            uint16_t c = sheet->palette[index];
            uint8_t r5 = (uint8_t)(c >> 11);
            uint8_t g6 = (uint8_t)((c >> 5) & 0x3F);
            uint8_t b5 = (uint8_t)(c & 0x1F);
            uint8_t bgra[4] = {
                (uint8_t)((b5 << 3) | (b5 >> 2)),
                (uint8_t)((g6 << 2) | (g6 >> 4)),
                (uint8_t)((r5 << 3) | (r5 >> 2)),
                0xFF,
            };

            for (uint32_t p = pos; p < pos + run && p < total; ++p) {
                uint32_t px =
                    (uint32_t)(sheet->y + p / sheet->w) * SPRITE_FRAME_W + sheet->x + p % sheet->w;

                memcpy(out + px * 4, bgra, sizeof(bgra));
            }
        }
        pos += run;
    }
}
