#include "screensaver_effects.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"

#define TAG "ss_effects"

/*
 * Bring-up placeholder effect. Proves the grid -> glyph -> writer -> color path
 * end to end before the real effects land. Replaced by the lightweight effects
 * in U4 (this entry is removed there).
 */
static void dots_render(void *ctx, pixel_writer_t writer, void *wctx, uint16_t cols, uint16_t rows,
                        uint32_t time_ms)
{
    uint32_t phase = time_ms / 80U;
    uint16_t color = 0x4208U; /* dim gray (RGB565) */

    (void)ctx;
    for (uint16_t r = 0; r < rows; r++) {
        for (uint16_t c = 0; c < cols; c++) {
            if (((c + r + phase) & 7U) == 0U) {
                ss_glyph_draw_char(writer, wctx, (int32_t)c * SS_CELL_W, (int32_t)r * SS_CELL_H, 1,
                                   '.', color);
            }
        }
    }
}

static const screensaver_effect_t k_dots = {
    .name = "dots",
    .ctx_size = 0,
    .reset = NULL,
    .render = dots_render,
};

static const screensaver_effect_t *const s_registry[] = {
    &k_dots,
};

static void *s_ctx; /* shared per-effect state buffer (max ctx_size) */
static size_t s_ctx_cap;
static int s_current = -1;
static int s_last = -1;

static int registry_count(void) { return (int)(sizeof(s_registry) / sizeof(s_registry[0])); }

bool screensaver_effects_init(void)
{
    size_t max_ctx = 0;

    for (int i = 0; i < registry_count(); i++) {
        if (s_registry[i]->ctx_size > max_ctx) {
            max_ctx = s_registry[i]->ctx_size;
        }
    }

    if (max_ctx > 0 && s_ctx == NULL) {
        s_ctx = heap_caps_malloc(max_ctx, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_ctx == NULL) {
            s_ctx = heap_caps_malloc(max_ctx, MALLOC_CAP_8BIT);
        }
        if (s_ctx == NULL) {
            ESP_LOGW(TAG, "effect ctx alloc failed (%u bytes)", (unsigned)max_ctx);
            return false;
        }
    }

    s_ctx_cap = max_ctx;
    s_current = -1;
    s_last = -1;
    return true;
}

void screensaver_effects_deinit(void)
{
    if (s_ctx != NULL) {
        heap_caps_free(s_ctx);
        s_ctx = NULL;
    }
    s_ctx_cap = 0;
    s_current = -1;
    s_last = -1;
}

int screensaver_effects_count(void) { return registry_count(); }

int screensaver_effects_select(uint16_t cols, uint16_t rows)
{
    int n = registry_count();
    int idx;
    const screensaver_effect_t *fx;

    if (n <= 0) {
        s_current = -1;
        return -1;
    }

    if (n == 1) {
        idx = 0; /* single effect: no-repeat constraint relaxed */
    } else {
        do {
            idx = (int)(esp_random() % (uint32_t)n);
        } while (idx == s_last);
    }

    s_current = idx;
    s_last = idx;

    fx = s_registry[idx];
    if (fx->reset != NULL && fx->ctx_size <= s_ctx_cap && s_ctx != NULL) {
        memset(s_ctx, 0, fx->ctx_size);
        fx->reset(s_ctx, cols, rows);
    }
    return idx;
}

void screensaver_effects_render(pixel_writer_t writer, void *writer_ctx, uint16_t cols,
                                uint16_t rows, uint32_t time_ms)
{
    if (s_current < 0 || writer == NULL) {
        return;
    }
    s_registry[s_current]->render(s_ctx, writer, writer_ctx, cols, rows, time_ms);
}

const char *screensaver_effects_current_name(void)
{
    return (s_current >= 0) ? s_registry[s_current]->name : "none";
}
