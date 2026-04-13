/*
 * CPU port of docs/plan/screensaver.glsl.
 *
 * The original effect is a rotated, wavy four-color gradient driven by a
 * low-frequency noise field. Here we keep that visual structure, but we fit it
 * to the existing low-resolution screensaver pipeline so both LVGL fallback and
 * direct mode can share the same renderer.
 */

#include "screensaver_renderer.h"

#include <math.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"

#define TAG "ss_renderer"

#define Q12 4096
#define FP_FROM_FLOAT(f) ((int32_t)((f) * Q12))
#define FP_MUL(a, b) ((int32_t)(((int64_t)(a) * (int64_t)(b)) / Q12))

#define NOISE_LUT_BITS 6
#define NOISE_LUT_SIZE (1U << NOISE_LUT_BITS)
#define NOISE_TIME_DIV_MS 10000U
#define BASE_ROTATE_DEG 180
#define PROJ_ROTATE_DEG (-5)
#define SPEED_DEG_PER_S 115
#define WAVE_X_DEG_PER_UNIT 286
#define WAVE_Y_DEG_PER_UNIT 430
#define WAVE_X_DIVISOR 30
#define WAVE_Y_DIVISOR 15
#define LAYER_GRAD_EDGE0_Q12 FP_FROM_FLOAT(-0.44f)
#define LAYER_GRAD_EDGE1_Q12 FP_FROM_FLOAT(0.34f)
#define LAYER_MIX_EDGE0_Q12 FP_FROM_FLOAT(0.62f)
#define LAYER_MIX_EDGE1_Q12 FP_FROM_FLOAT(-0.42f)
#define LAYER_DRIFT_STRENGTH_Q12 FP_FROM_FLOAT(0.11f)
#define MIX_DRIFT_STRENGTH_Q12 FP_FROM_FLOAT(0.08f)
#define EDGE_MIST_STRENGTH_Q12 FP_FROM_FLOAT(0.18f)
#define CROSS_MIST_STRENGTH_Q12 FP_FROM_FLOAT(0.14f)

#define COLOR_YELLOW_R 241
#define COLOR_YELLOW_G 206
#define COLOR_YELLOW_B 169

#define COLOR_DEEP_BLUE_R 78
#define COLOR_DEEP_BLUE_G 126
#define COLOR_DEEP_BLUE_B 232

#define COLOR_RED_R 223
#define COLOR_RED_G 144
#define COLOR_RED_B 206

#define COLOR_BLUE_R 110
#define COLOR_BLUE_G 189
#define COLOR_BLUE_B 239

#define COLOR_MIST_R 186
#define COLOR_MIST_G 180
#define COLOR_MIST_B 226

typedef struct {
    int16_t uv_x_q12;
    int16_t uv_y_q12;
    int16_t uv_y_iso_q12;
    int16_t noise_y_q12;
} renderer_pixel_t;

static renderer_pixel_t *s_pixels;
static uint8_t s_noise_lut[NOISE_LUT_SIZE * NOISE_LUT_SIZE];
static uint16_t s_width;
static uint16_t s_height;
static int16_t s_ratio_q12;
static int16_t s_proj_cos_q12;
static int16_t s_proj_sin_q12;
static bool s_initialized;
static bool s_noise_ready;

static inline int16_t wrap_deg(int32_t deg)
{
    deg %= 360;
    if (deg < 0) {
        deg += 360;
    }
    return (int16_t)deg;
}

static inline int16_t sin_q12(int32_t deg) { return (int16_t)(lv_trigo_sin(wrap_deg(deg)) >> 3); }

static inline int16_t cos_q12(int32_t deg) { return sin_q12(deg + 90); }

static inline int32_t clamp_q12_01(int32_t value)
{
    if (value < 0) {
        return 0;
    }
    if (value > Q12) {
        return Q12;
    }
    return value;
}

static inline uint8_t mix_u8(uint8_t a, uint8_t b, int32_t t_q12)
{
    return (uint8_t)(a + (((int32_t)b - (int32_t)a) * t_q12 + (Q12 / 2)) / Q12);
}

static inline int32_t noise_u8_to_signed_q12(uint8_t value)
{
    return (int32_t)(((int32_t)value * (2 * Q12) + 127) / 255) - Q12;
}

static inline int32_t midpoint_emphasis_q12(int32_t value_q12)
{
    int32_t centered = value_q12 * 2 - Q12;

    if (centered < 0) {
        centered = -centered;
    }
    if (centered >= Q12) {
        return 0;
    }

    return Q12 - centered;
}

static inline uint8_t wrap_noise_coord(int32_t value)
{
    value %= (int32_t)NOISE_LUT_SIZE;
    if (value < 0) {
        value += (int32_t)NOISE_LUT_SIZE;
    }
    return (uint8_t)value;
}

static int32_t floor_div_q12(int32_t value)
{
    if (value >= 0) {
        return value / Q12;
    }

    return -(((-value) + Q12 - 1) / Q12);
}

static int32_t smoothstep_q12(int32_t edge0_q12, int32_t edge1_q12, int32_t x_q12)
{
    int32_t denom = edge1_q12 - edge0_q12;
    int32_t t_q12;

    if (denom == 0) {
        return (x_q12 >= edge1_q12) ? Q12 : 0;
    }

    t_q12 = (int32_t)(((int64_t)(x_q12 - edge0_q12) * Q12) / denom);
    t_q12 = clamp_q12_01(t_q12);
    return FP_MUL(FP_MUL(t_q12, t_q12), 3 * Q12 - 2 * t_q12);
}

static uint32_t hash_u32(uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7feb352dU;
    value ^= value >> 15;
    value *= 0x846ca68bU;
    value ^= value >> 16;
    return value;
}

static void ensure_noise_lut(void)
{
    if (s_noise_ready) {
        return;
    }

    for (uint32_t y = 0; y < NOISE_LUT_SIZE; y++) {
        for (uint32_t x = 0; x < NOISE_LUT_SIZE; x++) {
            uint32_t seed = x * 0x9e3779b9U ^ y * 0x85ebca6bU ^ 0x243f6a88U;
            s_noise_lut[y * NOISE_LUT_SIZE + x] = (uint8_t)(hash_u32(seed) >> 24);
        }
    }

    s_noise_ready = true;
}

static uint8_t sample_noise_u8(int32_t x_q12, int32_t y_q12)
{
    int32_t x0 = floor_div_q12(x_q12);
    int32_t y0 = floor_div_q12(y_q12);
    int32_t fx_q12 = x_q12 - x0 * Q12;
    int32_t fy_q12 = y_q12 - y0 * Q12;
    int32_t x1 = x0 + 1;
    int32_t y1 = y0 + 1;
    int32_t v00 =
        s_noise_lut[(uint32_t)wrap_noise_coord(y0) * NOISE_LUT_SIZE + wrap_noise_coord(x0)];
    int32_t v10 =
        s_noise_lut[(uint32_t)wrap_noise_coord(y0) * NOISE_LUT_SIZE + wrap_noise_coord(x1)];
    int32_t v01 =
        s_noise_lut[(uint32_t)wrap_noise_coord(y1) * NOISE_LUT_SIZE + wrap_noise_coord(x0)];
    int32_t v11 =
        s_noise_lut[(uint32_t)wrap_noise_coord(y1) * NOISE_LUT_SIZE + wrap_noise_coord(x1)];
    int32_t nx0 = v00 + (((v10 - v00) * fx_q12) + (Q12 / 2)) / Q12;
    int32_t nx1 = v01 + (((v11 - v01) * fx_q12) + (Q12 / 2)) / Q12;

    return (uint8_t)(nx0 + (((nx1 - nx0) * fy_q12) + (Q12 / 2)) / Q12);
}

static void render_fallback_background(lv_color32_t *pixels, uint32_t stride_px, uint32_t time_ms)
{
    uint32_t phase = time_ms / 24U;

    if (pixels == NULL) {
        return;
    }

    for (uint16_t y = 0; y < s_height; y++) {
        int32_t blend_q12 = (s_height > 1) ? (int32_t)(((int64_t)y * Q12) / (s_height - 1)) : 0;
        int32_t blend_soft_q12 = FP_MUL(midpoint_emphasis_q12(blend_q12), CROSS_MIST_STRENGTH_Q12);

        for (uint16_t x = 0; x < s_width; x++) {
            int32_t band_q12 = smoothstep_q12(
                LAYER_GRAD_EDGE0_Q12, LAYER_GRAD_EDGE1_Q12,
                FP_FROM_FLOAT(((float)(((int32_t)x + (int32_t)phase) & 0xFF) / 255.0f) - 0.25f));
            uint8_t layer1_r = mix_u8(COLOR_YELLOW_R, COLOR_DEEP_BLUE_R, band_q12);
            uint8_t layer1_g = mix_u8(COLOR_YELLOW_G, COLOR_DEEP_BLUE_G, band_q12);
            uint8_t layer1_b = mix_u8(COLOR_YELLOW_B, COLOR_DEEP_BLUE_B, band_q12);
            uint8_t layer2_r = mix_u8(COLOR_RED_R, COLOR_BLUE_R, band_q12);
            uint8_t layer2_g = mix_u8(COLOR_RED_G, COLOR_BLUE_G, band_q12);
            uint8_t layer2_b = mix_u8(COLOR_RED_B, COLOR_BLUE_B, band_q12);
            uint8_t final_r;
            uint8_t final_g;
            uint8_t final_b;

            final_r = mix_u8(layer1_r, layer2_r, blend_q12);
            final_g = mix_u8(layer1_g, layer2_g, blend_q12);
            final_b = mix_u8(layer1_b, layer2_b, blend_q12);

            pixels[y * stride_px + x] =
                lv_color32_make(mix_u8(final_r, COLOR_MIST_R, blend_soft_q12),
                                mix_u8(final_g, COLOR_MIST_G, blend_soft_q12),
                                mix_u8(final_b, COLOR_MIST_B, blend_soft_q12), 0xFF);
        }
    }
}

bool screensaver_renderer_init(uint16_t width, uint16_t height)
{
    size_t table_size;

    if (width == 0 || height == 0) {
        return false;
    }

    ensure_noise_lut();
    screensaver_renderer_deinit();

    s_width = width;
    s_height = height;
    s_ratio_q12 = (int16_t)(((uint32_t)width * Q12 + (height / 2U)) / height);
    s_proj_cos_q12 = cos_q12(PROJ_ROTATE_DEG);
    s_proj_sin_q12 = sin_q12(PROJ_ROTATE_DEG);

    table_size = (size_t)width * height * sizeof(renderer_pixel_t);
    s_pixels = heap_caps_malloc(table_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_pixels == NULL) {
        ESP_LOGW(TAG, "PSRAM alloc failed (%u bytes), trying SRAM", (unsigned)table_size);
        s_pixels = heap_caps_malloc(table_size, MALLOC_CAP_8BIT);
    }
    if (s_pixels == NULL) {
        ESP_LOGE(TAG, "renderer table alloc failed");
        return false;
    }

    for (uint16_t y = 0; y < height; y++) {
        for (uint16_t x = 0; x < width; x++) {
            renderer_pixel_t *pixel = &s_pixels[y * width + x];
            float uv_x = (((float)x + 0.5f) / (float)width) - 0.5f;
            float uv_y = (((float)y + 0.5f) / (float)height) - 0.5f;
            float uv_y_iso = uv_y * ((float)height / (float)width);
            float uv_prod = uv_x * uv_y;

            pixel->uv_x_q12 = (int16_t)lrintf(uv_x * (float)Q12);
            pixel->uv_y_q12 = (int16_t)lrintf(uv_y * (float)Q12);
            pixel->uv_y_iso_q12 = (int16_t)lrintf(uv_y_iso * (float)Q12);
            pixel->noise_y_q12 = (int16_t)lrintf(uv_prod * (float)Q12);
        }
    }

    s_initialized = true;
    ESP_LOGI(TAG, "init ok: %ux%u", width, height);
    return true;
}

void screensaver_renderer_deinit(void)
{
    if (s_pixels != NULL) {
        heap_caps_free(s_pixels);
        s_pixels = NULL;
    }

    s_width = 0;
    s_height = 0;
    s_ratio_q12 = 0;
    s_proj_cos_q12 = 0;
    s_proj_sin_q12 = 0;
    s_initialized = false;
}

void screensaver_renderer_render(lv_color32_t *pixels, uint32_t stride_px, uint32_t time_ms)
{
    int32_t noise_time_q12;
    int32_t speed_deg;

    if (pixels == NULL) {
        return;
    }

    if (!s_initialized || s_pixels == NULL) {
        render_fallback_background(pixels, stride_px, time_ms);
        return;
    }

    noise_time_q12 =
        (int32_t)(((uint64_t)time_ms * Q12 + (NOISE_TIME_DIV_MS / 2U)) / NOISE_TIME_DIV_MS);
    speed_deg = (int32_t)(((uint64_t)time_ms * SPEED_DEG_PER_S + 500U) / 1000U);

    for (uint16_t y = 0; y < s_height; y++) {
        const renderer_pixel_t *row = &s_pixels[y * s_width];

        for (uint16_t x = 0; x < s_width; x++) {
            const renderer_pixel_t *pixel = &row[x];
            uint8_t degree_u8 = sample_noise_u8(noise_time_q12, pixel->noise_y_q12);
            int32_t degree_noise_q12 = noise_u8_to_signed_q12(degree_u8);
            int32_t rotate_deg = BASE_ROTATE_DEG + (((int32_t)degree_u8 - 128) * 720 + 128) / 256;
            int32_t rotate_sin_q12 = sin_q12(rotate_deg);
            int32_t rotate_cos_q12 = cos_q12(rotate_deg);
            int32_t tuv_x_q12 = FP_MUL(pixel->uv_x_q12, rotate_cos_q12) -
                                FP_MUL(pixel->uv_y_iso_q12, rotate_sin_q12);
            int32_t tuv_y_iso_q12 = FP_MUL(pixel->uv_x_q12, rotate_sin_q12) +
                                    FP_MUL(pixel->uv_y_iso_q12, rotate_cos_q12);
            int32_t tuv_y_q12 = FP_MUL(tuv_y_iso_q12, s_ratio_q12);
            int32_t wave1_deg = speed_deg + (tuv_y_q12 * WAVE_X_DEG_PER_UNIT) / Q12;
            int32_t wave2_deg;
            int32_t wave_x_q12;
            int32_t wave_y_q12;
            int32_t layer_grad_source_q12;
            int32_t layer_mix_source_q12;
            int32_t layer_grad_q12;
            int32_t layer_mix_q12;
            int32_t edge_bleed_q12;
            int32_t cross_bleed_q12;
            int32_t final_mist_q12;
            uint8_t layer1_r;
            uint8_t layer1_g;
            uint8_t layer1_b;
            uint8_t layer2_r;
            uint8_t layer2_g;
            uint8_t layer2_b;
            uint8_t final_r;
            uint8_t final_g;
            uint8_t final_b;

            wave_x_q12 = sin_q12(wave1_deg);
            tuv_x_q12 += wave_x_q12 / WAVE_X_DIVISOR;
            wave2_deg = speed_deg + (tuv_x_q12 * WAVE_Y_DEG_PER_UNIT) / Q12;
            wave_y_q12 = sin_q12(wave2_deg);
            tuv_y_q12 += wave_y_q12 / WAVE_Y_DIVISOR;

            layer_grad_source_q12 =
                FP_MUL(tuv_x_q12, s_proj_cos_q12) - FP_MUL(tuv_y_q12, s_proj_sin_q12);
            layer_grad_source_q12 += FP_MUL(degree_noise_q12, LAYER_DRIFT_STRENGTH_Q12);
            layer_mix_source_q12 =
                tuv_y_q12 + FP_MUL((wave_x_q12 + wave_y_q12) / 2, MIX_DRIFT_STRENGTH_Q12);

            layer_grad_q12 =
                smoothstep_q12(LAYER_GRAD_EDGE0_Q12, LAYER_GRAD_EDGE1_Q12, layer_grad_source_q12);
            layer_mix_q12 =
                smoothstep_q12(LAYER_MIX_EDGE0_Q12, LAYER_MIX_EDGE1_Q12, layer_mix_source_q12);

            edge_bleed_q12 = FP_MUL(midpoint_emphasis_q12(layer_grad_q12), EDGE_MIST_STRENGTH_Q12);
            cross_bleed_q12 = FP_MUL(midpoint_emphasis_q12(layer_mix_q12), CROSS_MIST_STRENGTH_Q12);
            final_mist_q12 = clamp_q12_01(cross_bleed_q12 + edge_bleed_q12 / 2);

            layer1_r = mix_u8(COLOR_YELLOW_R, COLOR_DEEP_BLUE_R, layer_grad_q12);
            layer1_g = mix_u8(COLOR_YELLOW_G, COLOR_DEEP_BLUE_G, layer_grad_q12);
            layer1_b = mix_u8(COLOR_YELLOW_B, COLOR_DEEP_BLUE_B, layer_grad_q12);

            layer2_r = mix_u8(COLOR_RED_R, COLOR_BLUE_R, layer_grad_q12);
            layer2_g = mix_u8(COLOR_RED_G, COLOR_BLUE_G, layer_grad_q12);
            layer2_b = mix_u8(COLOR_RED_B, COLOR_BLUE_B, layer_grad_q12);

            final_r = mix_u8(layer1_r, layer2_r, layer_mix_q12);
            final_g = mix_u8(layer1_g, layer2_g, layer_mix_q12);
            final_b = mix_u8(layer1_b, layer2_b, layer_mix_q12);

            pixels[y * stride_px + x] =
                lv_color32_make(mix_u8(final_r, COLOR_MIST_R, final_mist_q12),
                                mix_u8(final_g, COLOR_MIST_G, final_mist_q12),
                                mix_u8(final_b, COLOR_MIST_B, final_mist_q12), 0xFF);
        }
    }
}

void screensaver_renderer_get_dimensions(uint16_t *width, uint16_t *height)
{
    if (width != NULL) {
        *width = s_width;
    }
    if (height != NULL) {
        *height = s_height;
    }
}
