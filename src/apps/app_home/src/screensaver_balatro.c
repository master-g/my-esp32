/*
 * Balatro-inspired screensaver renderer — CPU implementation.
 *
 * Approximates the Balatro background shader on ESP32-S3 using:
 *   - Precomputed polar coordinates (one-time float init, stored as Q8.8)
 *   - lv_trigo_sin() LUT for all runtime trig (91-entry, Q15 output)
 *   - 3-iteration flow loop (vs original 5) for CPU budget
 *   - Original three-color palette mapping
 *
 * Target: < 5 ms per frame at 160x43, leaving headroom for LVGL compositing.
 */

#include "screensaver_balatro.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

#define TAG "balatro"
#define BALATRO_MOCK_RENDERER 0

/* ---- Balatro palette (original shader, 0-255) ---- */
#define C1_R 222
#define C1_G 68
#define C1_B 59

#define C2_R 0
#define C2_G 107
#define C2_B 180

#define C3_R 22
#define C3_G 35
#define C3_B 37

/* Shader constants */
#define CONTRAST 3.5f
#define LIGHTING 0.4f
#define SPIN_AMOUNT 0.25f
#define SPIN_SPEED 7.0f
#define SPIN_ROTATION (-2.0f)
#define SPIN_EASE 1.0f
#define BALATRO_FLOW_ITERS 2
#define BALATRO_TIME1_MDEG_PER_S 52589U
#define BALATRO_TIME2_MDEG_PER_S 45321U
#define BALATRO_TIME_SCALE_DEN 1000000U

/* Q8.8 fixed-point helpers */
#define Q8 256
#define FP_FROM_FLOAT(f) ((int16_t)((f) * Q8))
#define FP_MUL(a, b) ((int32_t)(a) * (int32_t)(b) / Q8)

/* Precomputed per-pixel polar data */
typedef struct {
    int16_t x_q8;      /* normalized x, Q8.8, roughly [-128, 127] after *30 */
    int16_t y_q8;      /* normalized y, Q8.8 */
    int16_t radius_q8; /* sqrt(x²+y²), Q8.8, before *30 scaling */
    int16_t angle_deg; /* atan2 result, 0-359 */
} pixel_polar_t;

static pixel_polar_t *s_polar_table;
static uint16_t s_width;
static uint16_t s_height;
static bool s_initialized;

/* ---- LUT-based trig wrappers ---- */

/* Wrap angle to 0..359 */
static inline int16_t wrap_deg(int32_t d)
{
    d %= 360;
    if (d < 0) {
        d += 360;
    }
    return (int16_t)d;
}

/* sin returning Q8.8 fixed-point (range ~ [-256, 255]) */
static inline int16_t sin_q8(int32_t deg)
{
    int32_t q15 = lv_trigo_sin(wrap_deg(deg)); /* [-32768, 32767] */
    return (int16_t)(q15 >> 7);                /* Q15 -> Q8.8 */
}

/* cos returning Q8.8 */
static inline int16_t cos_q8(int32_t deg) { return sin_q8(deg + 90); }

static inline uint8_t clamp_u8(int32_t v)
{
    if (v < 0)
        return 0;
    if (v > 255)
        return 255;
    return (uint8_t)v;
}

static uint32_t isqrt32(uint32_t value)
{
    uint32_t root = 0;
    uint32_t bit = 1UL << 30;

    while (bit > value) {
        bit >>= 2;
    }

    while (bit != 0) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }

    return root;
}

/* ---- Init / deinit ---- */

bool balatro_init(uint16_t width, uint16_t height)
{
    if (s_initialized) {
        balatro_deinit();
    }

    s_width = width;
    s_height = height;

    size_t table_size = (size_t)width * height * sizeof(pixel_polar_t);
    s_polar_table = heap_caps_malloc(table_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_polar_table == NULL) {
        ESP_LOGW(TAG, "PSRAM alloc failed (%u bytes), trying SRAM", (unsigned)table_size);
        s_polar_table = heap_caps_malloc(table_size, MALLOC_CAP_8BIT);
    }
    if (s_polar_table == NULL) {
        ESP_LOGE(TAG, "polar table alloc failed");
        return false;
    }

    /* Screen diagonal for normalization (matches shader's length(screenSize)) */
    float diag = sqrtf((float)(width * width + height * height));
    float mid_x = ((float)width / diag) * 0.5f;
    float mid_y = ((float)height / diag) * 0.5f;

    /* Constant angular offset from the shader (non-rotating mode):
     * speed = SPIN_ROTATION * SPIN_EASE * 0.2 + 302.2 = -0.4 + 302.2 = 301.8 */
    float base_speed = SPIN_ROTATION * SPIN_EASE * 0.2f + 302.2f;

    for (uint16_t y = 0; y < height; y++) {
        for (uint16_t x = 0; x < width; x++) {
            pixel_polar_t *p = &s_polar_table[y * width + x];

            /* Shader UV normalization (pixelation step is implicit via low-res canvas) */
            float ux = ((float)x - 0.5f * (float)width) / diag;
            float uy = ((float)y - 0.5f * (float)height) / diag;
            float r = sqrtf(ux * ux + uy * uy);
            float angle_rad = atan2f(uy, ux);

            /* Polar spin warp from shader */
            float new_angle = angle_rad + base_speed -
                              SPIN_EASE * 20.0f * (SPIN_AMOUNT * r + (1.0f - SPIN_AMOUNT));

            float warped_x = r * cosf(new_angle) + mid_x - mid_x;
            float warped_y = r * sinf(new_angle) + mid_y - mid_y;

            /* Scale by 30 as the shader does */
            warped_x *= 30.0f;
            warped_y *= 30.0f;

            /* Store as Q8.8 */
            p->x_q8 = FP_FROM_FLOAT(warped_x);
            p->y_q8 = FP_FROM_FLOAT(warped_y);
            p->radius_q8 = FP_FROM_FLOAT(r);

            /* Angle in degrees for later LUT use */
            float angle_deg_f = angle_rad * (180.0f / 3.14159265f);
            if (angle_deg_f < 0)
                angle_deg_f += 360.0f;
            p->angle_deg = (int16_t)angle_deg_f;
        }
    }

    s_initialized = true;
    ESP_LOGI(TAG, "init ok: %ux%u, polar table %u bytes in %s", width, height, (unsigned)table_size,
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 0 ? "PSRAM" : "SRAM");
    return true;
}

void balatro_deinit(void)
{
    if (s_polar_table != NULL) {
        heap_caps_free(s_polar_table);
        s_polar_table = NULL;
    }
    s_initialized = false;
}

static void render_mock_background(lv_color32_t *pixels, uint32_t stride_px, uint32_t time_ms)
{
    if (pixels == NULL) {
        return;
    }

    int32_t anim_phase = (int32_t)(time_ms / 16U);

    for (uint16_t y = 0; y < s_height; y++) {
        int32_t row_phase = (int32_t)y * 6 + anim_phase * 9;

        for (uint16_t x = 0; x < s_width; x++) {
            int32_t diag = ((int32_t)x * 7 + row_phase) & 0xFF;
            int32_t cross = (((int32_t)x * 5 - (int32_t)y * 3) + anim_phase * 11) & 0x3F;
            uint8_t r = (uint8_t)(18 + (diag >> 2) + (cross >> 3));
            uint8_t g = (uint8_t)(14 + (cross >> 1));
            uint8_t b = (uint8_t)(32 + (diag >> 1));

            pixels[y * stride_px + x] = lv_color32_make(r, g, b, 0xFF);
        }
    }
}

/* ---- Frame rendering ---- */

void balatro_render(lv_color32_t *pixels, uint32_t stride_px, uint32_t time_ms)
{
    if (pixels == NULL) {
        return;
    }

#if BALATRO_MOCK_RENDERER
    render_mock_background(pixels, stride_px, time_ms);
    return;
#endif

    if (!s_initialized || s_polar_table == NULL) {
        render_mock_background(pixels, stride_px, time_ms);
        return;
    }

    /* Drive the flow with monotonic real time so the pattern does not collapse
     * into a short exact loop. These two terms come directly from the original
     * shader's iTime-based offsets, converted to degrees per second. */
    int32_t speed_term1_base =
        (int32_t)(((uint64_t)time_ms * BALATRO_TIME1_MDEG_PER_S) / BALATRO_TIME_SCALE_DEN);
    int32_t speed_term2_base =
        (int32_t)(((uint64_t)time_ms * BALATRO_TIME2_MDEG_PER_S) / BALATRO_TIME_SCALE_DEN);

    /* Contrast modifier (constant from shader) */
    /* contrast_mod = 0.25 * 3.5 + 0.5 * 0.25 + 1.2 = 0.875 + 0.125 + 1.2 = 2.2 */
    /* In Q8.8: 2.2 * 256 ≈ 563 */
    int32_t contrast_mod_q8 = 563;

    /* Pre-blended base color: (0.3/CONTRAST) * COLOUR_1
     * 0.3/3.5 ≈ 0.0857
     * R: 222 * 0.0857 ≈ 19, G: 68 * 0.0857 ≈ 6, B: 59 * 0.0857 ≈ 5 */
    int32_t base_r = 19;
    int32_t base_g = 6;
    int32_t base_b = 5;

    /* Palette scale: (1 - 0.3/CONTRAST) ≈ 0.9143 → Q8.8 ≈ 234 */
    int32_t pal_scale_q8 = 234;

    for (uint16_t y = 0; y < s_height; y++) {
        const pixel_polar_t *row = &s_polar_table[y * s_width];

        for (uint16_t x = 0; x < s_width; x++) {
            const pixel_polar_t *pp = &row[x];

            /* Start from precomputed warped UV (already includes polar spin + *30) */
            int32_t ux = pp->x_q8;
            int32_t uy = pp->y_q8;

            /* uv2 is a vec2 in the original shader — both components start as
             * (uv.x + uv.y) but diverge as uv2.x += uv.x and uv2.y += uv.y
             * each iteration. Track x/y separately for proper asymmetry. */
            int32_t uv2_x = ux + uy;
            int32_t uv2_y = ux + uy;

            /* Flow distortion loop — reduced iterations for the ESP32 direct path.
             * Uses LUT sin/cos. We work in Q8.8 throughout.
             *
             * Original per iteration:
             *   uv2 += sin(max(uv.x, uv.y)) + uv
             *   uv  += 0.5 * vec2(cos(5.1123314 + 0.353*uv2.y + speed*0.131121),
             *                      sin(uv2.x - 0.113*speed))
             *   uv  -= cos(uv.x + uv.y) - sin(uv.x*0.711 - uv.y)
             */
            for (int i = 0; i < BALATRO_FLOW_ITERS; i++) {
                /* sin(max(ux,uy)) — convert Q8.8 to degrees for LUT.
                 * Conversion: value_q8 * (180/π) / 256 = value_q8 * 57 / 256 */
                int32_t max_uv = (ux > uy) ? ux : uy;
                int32_t max_deg = (max_uv * 57) >> 8;
                int32_t sin_max = sin_q8(max_deg);

                uv2_x += sin_max + ux;
                uv2_y += sin_max + uy;

                /* Convert uv2 components and speed to degree arguments for trig.
                 * 5.1123314 rad ≈ 293°
                 * 0.353 * uv2.y → uv2_y_deg * 0.353, approximated as * 90 / 256
                 * The original shader uses elapsed iTime here. We pre-convert that
                 * to degree offsets once per frame and feed the LUT with wrapped ints. */
                int32_t uv2_y_deg = (uv2_y * 57) >> 8;
                int32_t uv2_x_deg = (uv2_x * 57) >> 8;
                int32_t speed_term1 = 293 + (uv2_y_deg * 90) / Q8 + speed_term1_base;
                int32_t speed_term2 = uv2_x_deg - speed_term2_base;

                ux += cos_q8(speed_term1) / 2;
                uy += sin_q8(speed_term2) / 2;

                /* uv -= cos(ux+uy) - sin(ux*0.711 - uy) */
                int32_t sum_deg = ((ux + uy) * 57) >> 8;
                /* 0.711 ≈ 182/256 in Q8.8 */
                int32_t diff_deg = ((FP_MUL(ux, 182) - uy) * 57) >> 8;
                int32_t flow_term = cos_q8(sum_deg) - sin_q8(diff_deg);
                ux -= flow_term;
                uy -= flow_term;
            }

            /* Palette mapping.
             * paint_res = clamp(length(uv) * 0.035 * contrast_mod, 0, 2)
             *
             * Use an integer sqrt here; the cheaper manhattan proxy drifts the
             * palette too far toward green/yellow in the outer field. */
            int32_t abs_ux = (ux < 0) ? -ux : ux;
            int32_t abs_uy = (uy < 0) ? -uy : uy;
            uint32_t len_sq = (uint32_t)((uint32_t)abs_ux * (uint32_t)abs_ux +
                                         (uint32_t)abs_uy * (uint32_t)abs_uy);
            int32_t len_q8 = (int32_t)isqrt32(len_sq);

            /* paint_res in Q8.8 = len_q8 * 0.035 * contrast_mod
             * 0.035 * 256 ≈ 9 → paint_res_q8 = len_q8 * 9 * contrast_mod_q8 / Q8² */
            int32_t paint_q8 = (len_q8 * 9) / Q8;
            paint_q8 = FP_MUL(paint_q8, contrast_mod_q8);
            if (paint_q8 < 0)
                paint_q8 = 0;
            if (paint_q8 > 2 * Q8)
                paint_q8 = 2 * Q8;

            /* c1p = max(0, 1 - contrast_mod * |1 - paint_res|)
             * c2p = max(0, 1 - contrast_mod * |paint_res|)
             * All in Q8.8. */
            int32_t one_q8 = Q8;
            int32_t diff1 = one_q8 - paint_q8;
            if (diff1 < 0)
                diff1 = -diff1;
            int32_t c1p_q8 = one_q8 - FP_MUL(contrast_mod_q8, diff1);
            if (c1p_q8 < 0)
                c1p_q8 = 0;
            if (c1p_q8 > Q8)
                c1p_q8 = Q8;

            int32_t diff2 = paint_q8;
            if (diff2 < 0)
                diff2 = -diff2;
            int32_t c2p_q8 = one_q8 - FP_MUL(contrast_mod_q8, diff2);
            if (c2p_q8 < 0)
                c2p_q8 = 0;
            if (c2p_q8 > Q8)
                c2p_q8 = Q8;

            int32_t c3p_q8 = one_q8 - c1p_q8 - c2p_q8;
            if (c3p_q8 < 0)
                c3p_q8 = 0;

            /* Lighting: light = 0.2 * max(c1p*5-4, 0) + 0.4 * max(c2p*5-4, 0) */
            int32_t light1 = c1p_q8 * 5 - 4 * Q8;
            if (light1 < 0)
                light1 = 0;
            int32_t light2 = c2p_q8 * 5 - 4 * Q8;
            if (light2 < 0)
                light2 = 0;
            /* light in Q8.8, scale 0.2 = 51/256, 0.4 = 102/256 */
            int32_t light_q8 = (light1 * 51 + light2 * 102) / Q8;

            /* Final color: base + pal_scale * (C1*c1p + C2*c2p + C3*c3p) + light
             * All channel calcs in 0-255 range. */
            int32_t r = base_r +
                        (pal_scale_q8 * (C1_R * c1p_q8 + C2_R * c2p_q8 + C3_R * c3p_q8) / Q8) / Q8 +
                        (light_q8 * 255) / Q8;
            int32_t g = base_g +
                        (pal_scale_q8 * (C1_G * c1p_q8 + C2_G * c2p_q8 + C3_G * c3p_q8) / Q8) / Q8 +
                        (light_q8 * 255) / Q8;
            int32_t b = base_b +
                        (pal_scale_q8 * (C1_B * c1p_q8 + C2_B * c2p_q8 + C3_B * c3p_q8) / Q8) / Q8 +
                        (light_q8 * 255) / Q8;

            pixels[y * stride_px + x] =
                lv_color32_make(clamp_u8(r), clamp_u8(g), clamp_u8(b), 0xFF);
        }
    }
}

void balatro_get_dimensions(uint16_t *width, uint16_t *height)
{
    if (width != NULL) {
        *width = s_width;
    }
    if (height != NULL) {
        *height = s_height;
    }
}
