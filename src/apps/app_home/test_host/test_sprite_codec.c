/* Host test for the Home sprite decoder, run by `make test-host`: every frame of
 * every generated sheet must decode to the bytes the converter expected. */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>

#include "generated/sprite_sheets_expected.h"

static uint32_t fnv1a(const uint8_t *data, size_t len)
{
    uint32_t hash = 0x811C9DC5u;

    for (size_t i = 0; i < len; ++i) {
        hash = (hash ^ data[i]) * 0x01000193u;
    }
    return hash;
}

int main(void)
{
    static uint8_t frame[SPRITE_FRAME_BYTES];
    size_t frames = 0;

    for (size_t s = 0; s < sizeof(sprite_expected) / sizeof(sprite_expected[0]); ++s) {
        const sprite_expected_t *expected = &sprite_expected[s];

        assert(expected->sheet->x + expected->sheet->w <= SPRITE_FRAME_W);
        assert(expected->sheet->y + expected->sheet->h <= SPRITE_FRAME_H);
        for (uint8_t f = 0; f < expected->sheet->frame_count; ++f) {
            sprite_decode_frame(expected->sheet, f, frame);
            if (fnv1a(frame, sizeof(frame)) != expected->frame_hashes[f]) {
                printf("%s frame %u decodes differently from the converter\n", expected->name, f);
                return 1;
            }
            frames++;
        }
    }
    printf("sprite codec host tests: ok (%zu frames)\n", frames);
    return 0;
}
