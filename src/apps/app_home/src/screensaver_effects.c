#include "screensaver_effects.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "lvgl.h"

#define TAG "ss_effects"

#define SS_MAX_COLS 80
#define SS_MAX_ROWS 16
#define SS_STARS 110
#define SS_DROPS 70
#define SS_PIPES 4

/* 10 density levels for plasma / flow shading. */
static const char SS_RAMP[] = " .:-=+*#%@";

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8U) << 8) | ((g & 0xFCU) << 3) | (b >> 3));
}

static inline int16_t wrap360(int32_t d)
{
    d %= 360;
    if (d < 0) {
        d += 360;
    }
    return (int16_t)d;
}

/* --- 1. Matrix character rain (per-column heads, time-based) --- */
static const char MATRIX_CH[] = "01<>*/=+#$&AMRX0123456789";

typedef struct {
    uint16_t offset[SS_MAX_COLS];
    uint8_t speed[SS_MAX_COLS];
} matrix_state_t;

static void matrix_reset(void *ctx, uint16_t cols, uint16_t rows)
{
    matrix_state_t *s = (matrix_state_t *)ctx;

    (void)cols;
    (void)rows;
    for (uint16_t c = 0; c < SS_MAX_COLS; c++) {
        s->offset[c] = (uint16_t)(esp_random() % 512U);
        s->speed[c] = (uint8_t)(4U + esp_random() % 12U);
    }
}

static void matrix_render(void *ctx, pixel_writer_t w, void *wctx, uint16_t cols, uint16_t rows,
                          uint32_t t)
{
    matrix_state_t *s = (matrix_state_t *)ctx;
    uint16_t period = (uint16_t)(rows + 8U);

    for (uint16_t col = 0; col < cols && col < SS_MAX_COLS; col++) {
        int32_t head = (int32_t)(((t * s->speed[col]) / 120U + s->offset[col]) % period);

        for (int32_t tail = 0; tail < 8; tail++) {
            int32_t r = head - tail;
            char ch;
            uint16_t color;

            if (r < 0 || r >= rows) {
                continue;
            }
            color =
                (tail == 0) ? rgb565(216, 255, 224) : rgb565(40, (uint8_t)(200 - tail * 22), 80);
            ch = MATRIX_CH[(t / 90U + col * 3U + (uint32_t)r) % (sizeof(MATRIX_CH) - 1U)];
            ss_glyph_draw_char(w, wctx, col * SS_CELL_W, r * SS_CELL_H, 1, ch, color);
        }
    }
}

/* --- 2. ASCII plasma / flow field (stateless) --- */
static void plasma_render(void *ctx, pixel_writer_t w, void *wctx, uint16_t cols, uint16_t rows,
                          uint32_t t)
{
    int32_t tt = (int32_t)(t / 16U);

    (void)ctx;
    for (uint16_t r = 0; r < rows; r++) {
        for (uint16_t col = 0; col < cols; col++) {
            int32_t v = lv_trigo_sin(wrap360(col * 14 + tt)) + lv_trigo_sin(wrap360(r * 20 - tt)) +
                        lv_trigo_sin(wrap360((col + r) * 9 + tt));
            int32_t norm = (v + 3 * 32767) / (6 * 32767 / 9 + 1);
            uint8_t g;

            if (norm <= 0) {
                continue;
            }
            if (norm > 9) {
                norm = 9;
            }
            g = (uint8_t)(80 + norm * 17);
            ss_glyph_draw_char(w, wctx, col * SS_CELL_W, r * SS_CELL_H, 1, SS_RAMP[norm],
                               rgb565(40, g, (uint8_t)(g - 20)));
        }
    }
}

/* --- 3. Sine wave bands (stateless) --- */
static void sine_render(void *ctx, pixel_writer_t w, void *wctx, uint16_t cols, uint16_t rows,
                        uint32_t t)
{
    static const char band[] = {'~', '-', '='};
    int32_t tt = (int32_t)(t / 24U);

    (void)ctx;
    for (int layer = 0; layer < 3; layer++) {
        for (uint16_t col = 0; col < cols; col++) {
            int32_t y = rows / 2 + (lv_trigo_sin(wrap360(col * 8 + tt + layer * 40)) * 3) / 32767 +
                        (lv_trigo_sin(wrap360(col * 3 - tt)) * 3) / 32767;

            if (y < 0 || y >= rows) {
                continue;
            }
            ss_glyph_draw_char(w, wctx, col * SS_CELL_W, (int32_t)y * SS_CELL_H, 1, band[layer],
                               rgb565((uint8_t)(60 + layer * 30), (uint8_t)(120 + layer * 40),
                                      (uint8_t)(200 - layer * 30)));
        }
    }
}

/* --- 4. Starfield: stars emanate from center (time-based radius) --- */
typedef struct {
    uint16_t angle[SS_STARS];
    uint16_t phase[SS_STARS];
} star_state_t;

static void star_reset(void *ctx, uint16_t cols, uint16_t rows)
{
    star_state_t *s = (star_state_t *)ctx;

    (void)cols;
    (void)rows;
    for (int i = 0; i < SS_STARS; i++) {
        s->angle[i] = (uint16_t)(esp_random() % 360U);
        s->phase[i] = (uint16_t)(esp_random() % 1024U);
    }
}

static void star_render(void *ctx, pixel_writer_t w, void *wctx, uint16_t cols, uint16_t rows,
                        uint32_t t)
{
    star_state_t *s = (star_state_t *)ctx;
    int32_t cx = cols / 2;
    int32_t cy = rows / 2;
    int32_t rmax = (cols > rows ? cols : rows);

    for (int i = 0; i < SS_STARS; i++) {
        int32_t rad = (int32_t)(((t / 14U) + s->phase[i]) % (uint32_t)(rmax + 1));
        int32_t col = cx + (lv_trigo_sin(wrap360(s->angle[i] + 90)) * rad) / 32767;
        int32_t row = cy + (lv_trigo_sin(wrap360(s->angle[i])) * rad) / 32767;
        char ch;

        if (col < 0 || row < 0 || col >= cols || row >= rows) {
            continue;
        }
        ch = (rad * 3 > rmax * 2) ? '*' : ((rad * 3 > rmax) ? '+' : '.');
        ss_glyph_draw_char(w, wctx, col * SS_CELL_W, row * SS_CELL_H, 1, ch, rgb565(200, 220, 255));
    }
}

/* --- 5. Diagonal rain (time-based drops) --- */
typedef struct {
    uint8_t x[SS_DROPS];
    uint8_t speed[SS_DROPS];
    uint16_t phase[SS_DROPS];
} rain_state_t;

static void rain_reset(void *ctx, uint16_t cols, uint16_t rows)
{
    rain_state_t *s = (rain_state_t *)ctx;

    (void)rows;
    for (int i = 0; i < SS_DROPS; i++) {
        s->x[i] = (uint8_t)(esp_random() % (cols > 0 ? cols : 1U));
        s->speed[i] = (uint8_t)(6U + esp_random() % 10U);
        s->phase[i] = (uint16_t)(esp_random() % 256U);
    }
}

static void rain_render(void *ctx, pixel_writer_t w, void *wctx, uint16_t cols, uint16_t rows,
                        uint32_t t)
{
    rain_state_t *s = (rain_state_t *)ctx;
    uint16_t period = (uint16_t)(rows + 2U);

    for (int i = 0; i < SS_DROPS; i++) {
        int32_t y = (int32_t)(((t * s->speed[i]) / 260U + s->phase[i]) % period);
        int32_t col = (int32_t)s->x[i] - y / 4;

        if (col < 0 || y < 0 || col >= cols || y >= rows) {
            continue;
        }
        ss_glyph_draw_char(w, wctx, col * SS_CELL_W, y * SS_CELL_H, 1, '/', rgb565(150, 180, 220));
    }
}

/* --- 6. Conway's Game of Life --- */
#define GOL_STEP_MS 130
typedef struct {
    uint8_t cell[SS_MAX_ROWS][SS_MAX_COLS];
    uint32_t last_step;
    uint32_t last_seed;
} gol_state_t;

static void gol_reset(void *ctx, uint16_t cols, uint16_t rows)
{
    gol_state_t *s = (gol_state_t *)ctx;

    (void)cols;
    (void)rows;
    for (int r = 0; r < SS_MAX_ROWS; r++) {
        for (int c = 0; c < SS_MAX_COLS; c++) {
            s->cell[r][c] = (esp_random() % 100U) < 32U ? 1U : 0U;
        }
    }
    s->last_step = 0;
    s->last_seed = 0;
}

static void gol_step(gol_state_t *s, uint16_t R, uint16_t C)
{
    static uint8_t next[SS_MAX_ROWS][SS_MAX_COLS];

    for (uint16_t r = 0; r < R; r++) {
        for (uint16_t c = 0; c < C; c++) {
            int n = 0;
            for (int dr = -1; dr <= 1; dr++) {
                for (int dc = -1; dc <= 1; dc++) {
                    if (dr == 0 && dc == 0) {
                        continue;
                    }
                    n += s->cell[(r + dr + R) % R][(c + dc + C) % C];
                }
            }
            next[r][c] = s->cell[r][c] ? (n == 2 || n == 3) : (n == 3);
        }
    }
    for (uint16_t r = 0; r < R; r++) {
        for (uint16_t c = 0; c < C; c++) {
            s->cell[r][c] = next[r][c];
        }
    }
}

static void gol_render(void *ctx, pixel_writer_t w, void *wctx, uint16_t cols, uint16_t rows,
                       uint32_t t)
{
    gol_state_t *s = (gol_state_t *)ctx;
    uint16_t R = rows < SS_MAX_ROWS ? rows : SS_MAX_ROWS;
    uint16_t C = cols < SS_MAX_COLS ? cols : SS_MAX_COLS;

    if (s->last_step == 0 || t - s->last_step >= GOL_STEP_MS) {
        gol_step(s, R, C);
        s->last_step = (t == 0) ? 1 : t;
    }
    if (t - s->last_seed >= 6000U) { /* perturb so the field never freezes */
        for (int i = 0; i < 24; i++) {
            s->cell[esp_random() % R][esp_random() % C] = 1;
        }
        s->last_seed = t;
    }
    for (uint16_t r = 0; r < R; r++) {
        for (uint16_t c = 0; c < C; c++) {
            if (s->cell[r][c]) {
                ss_glyph_draw_char(w, wctx, c * SS_CELL_W, r * SS_CELL_H, 1, '#',
                                   rgb565(120, 200, 255));
            }
        }
    }
}

/* --- 7. ASCII fire --- */
#define FIRE_STEP_MS 45
typedef struct {
    uint8_t heat[SS_MAX_ROWS + 1][SS_MAX_COLS];
    uint32_t last_step;
} fire_state_t;

static void fire_reset(void *ctx, uint16_t cols, uint16_t rows)
{
    fire_state_t *s = (fire_state_t *)ctx;

    (void)cols;
    (void)rows;
    memset(s->heat, 0, sizeof(s->heat));
    s->last_step = 0;
}

static void fire_step(fire_state_t *s, uint16_t R, uint16_t C)
{
    for (uint16_t c = 0; c < C; c++) {
        s->heat[R][c] = (esp_random() % 100U) < 80U ? 255U : 90U; /* source row */
    }
    for (int r = (int)R - 1; r >= 0; r--) {
        for (uint16_t c = 0; c < C; c++) {
            uint16_t l = (uint16_t)((c + C - 1) % C);
            uint16_t rr = (uint16_t)((c + 1) % C);
            int sum =
                s->heat[r + 1][l] + s->heat[r + 1][c] + s->heat[r + 1][rr] + s->heat[r + 1][c];
            int v = sum / 4 - 14;
            s->heat[r][c] = (uint8_t)(v < 0 ? 0 : v);
        }
    }
}

static void fire_render(void *ctx, pixel_writer_t w, void *wctx, uint16_t cols, uint16_t rows,
                        uint32_t t)
{
    fire_state_t *s = (fire_state_t *)ctx;
    uint16_t R = rows < SS_MAX_ROWS ? rows : SS_MAX_ROWS;
    uint16_t C = cols < SS_MAX_COLS ? cols : SS_MAX_COLS;

    if (s->last_step == 0 || t - s->last_step >= FIRE_STEP_MS) {
        fire_step(s, R, C);
        s->last_step = (t == 0) ? 1 : t;
    }
    for (uint16_t r = 0; r < R; r++) {
        for (uint16_t c = 0; c < C; c++) {
            uint8_t v = s->heat[r][c];
            int idx;
            uint8_t g;
            uint8_t b;

            if (v < 24) {
                continue;
            }
            idx = v * 9 / 255;
            if (idx > 9) {
                idx = 9;
            }
            g = (uint8_t)(v * 4 / 5);
            b = (uint8_t)(v > 180 ? (v - 180) * 3 : 0);
            ss_glyph_draw_char(w, wctx, c * SS_CELL_W, r * SS_CELL_H, 1, SS_RAMP[idx],
                               rgb565(255, g, b));
        }
    }
}

/* --- 8. Pipes (box-drawing growth) --- */
#define PIPES_STEP_MS 70
typedef struct {
    uint8_t glyph[SS_MAX_ROWS][SS_MAX_COLS]; /* (pipe_idx << 3) | box_code; 0 = empty */
    uint8_t cr[SS_PIPES];
    uint8_t cc[SS_PIPES];
    int8_t dr[SS_PIPES];
    int8_t dc[SS_PIPES];
    uint32_t last_step;
    uint16_t drawn;
} pipes_state_t;

/* cyan, magenta, green, amber (RGB565) */
static const uint16_t PIPE_PALETTE[SS_PIPES] = {0x56FF, 0xFBDB, 0x7FF4, 0xFE4B};

/* Box-drawing corner connecting the side we came from to the side we go to. */
static uint8_t pipe_corner(int8_t odc, int8_t odr, int8_t ndc, int8_t ndr)
{
    int sx = -odc + ndc;
    int sy = -odr + ndr;

    if (sx > 0 && sy > 0) {
        return SS_GLYPH_PIPE_TL;
    }
    if (sx < 0 && sy > 0) {
        return SS_GLYPH_PIPE_TR;
    }
    if (sx > 0 && sy < 0) {
        return SS_GLYPH_PIPE_BL;
    }
    return SS_GLYPH_PIPE_BR;
}

static void pipes_reset(void *ctx, uint16_t cols, uint16_t rows)
{
    pipes_state_t *s = (pipes_state_t *)ctx;

    memset(s->glyph, 0, sizeof(s->glyph));
    for (int i = 0; i < SS_PIPES; i++) {
        s->cr[i] = (uint8_t)(esp_random() % (rows > 0 ? rows : 1U));
        s->cc[i] = (uint8_t)(esp_random() % (cols > 0 ? cols : 1U));
        if (esp_random() & 1U) {
            s->dc[i] = (esp_random() & 1U) ? 1 : -1;
            s->dr[i] = 0;
        } else {
            s->dr[i] = (esp_random() & 1U) ? 1 : -1;
            s->dc[i] = 0;
        }
    }
    s->last_step = 0;
    s->drawn = 0;
}

static void pipes_step(pipes_state_t *s, uint16_t R, uint16_t C)
{
    if (s->drawn > (uint16_t)((uint32_t)R * C * 7U / 10U)) {
        memset(s->glyph, 0, sizeof(s->glyph));
        s->drawn = 0;
    }
    for (int i = 0; i < SS_PIPES; i++) {
        int8_t ndc = s->dc[i];
        int8_t ndr = s->dr[i];
        uint8_t code;

        if ((esp_random() % 100U) < 22U) {
            if (s->dc[i] != 0) {
                ndc = 0;
                ndr = (esp_random() & 1U) ? 1 : -1;
            } else {
                ndr = 0;
                ndc = (esp_random() & 1U) ? 1 : -1;
            }
        }
        code = (ndc == s->dc[i] && ndr == s->dr[i])
                   ? (uint8_t)(s->dc[i] != 0 ? SS_GLYPH_PIPE_H : SS_GLYPH_PIPE_V)
                   : pipe_corner(s->dc[i], s->dr[i], ndc, ndr);
        if (s->cr[i] < R && s->cc[i] < C) {
            if (s->glyph[s->cr[i]][s->cc[i]] == 0) {
                s->drawn++;
            }
            s->glyph[s->cr[i]][s->cc[i]] = (uint8_t)(((unsigned)i << 3) | code);
        }
        s->dc[i] = ndc;
        s->dr[i] = ndr;
        s->cc[i] = (uint8_t)(((int)s->cc[i] + ndc + C) % C);
        s->cr[i] = (uint8_t)(((int)s->cr[i] + ndr + R) % R);
    }
}

static void pipes_render(void *ctx, pixel_writer_t w, void *wctx, uint16_t cols, uint16_t rows,
                         uint32_t t)
{
    pipes_state_t *s = (pipes_state_t *)ctx;
    uint16_t R = rows < SS_MAX_ROWS ? rows : SS_MAX_ROWS;
    uint16_t C = cols < SS_MAX_COLS ? cols : SS_MAX_COLS;

    if (s->last_step == 0 || t - s->last_step >= PIPES_STEP_MS) {
        pipes_step(s, R, C);
        s->last_step = (t == 0) ? 1 : t;
    }
    for (uint16_t r = 0; r < R; r++) {
        for (uint16_t c = 0; c < C; c++) {
            uint8_t g = s->glyph[r][c];

            if (g == 0) {
                continue;
            }
            ss_glyph_draw_char(w, wctx, c * SS_CELL_W, r * SS_CELL_H, 1, (char)(g & 0x07U),
                               PIPE_PALETTE[(g >> 3) & 0x03U]);
        }
    }
}

static const screensaver_effect_t k_matrix = {.name = "matrix",
                                              .ctx_size = sizeof(matrix_state_t),
                                              .reset = matrix_reset,
                                              .render = matrix_render};
static const screensaver_effect_t k_plasma = {
    .name = "plasma", .ctx_size = 0, .reset = NULL, .render = plasma_render};
static const screensaver_effect_t k_sine = {
    .name = "sine", .ctx_size = 0, .reset = NULL, .render = sine_render};
static const screensaver_effect_t k_stars = {
    .name = "stars", .ctx_size = sizeof(star_state_t), .reset = star_reset, .render = star_render};
static const screensaver_effect_t k_rain = {
    .name = "rain", .ctx_size = sizeof(rain_state_t), .reset = rain_reset, .render = rain_render};

static const screensaver_effect_t k_gol = {
    .name = "life", .ctx_size = sizeof(gol_state_t), .reset = gol_reset, .render = gol_render};
static const screensaver_effect_t k_fire = {
    .name = "fire", .ctx_size = sizeof(fire_state_t), .reset = fire_reset, .render = fire_render};
static const screensaver_effect_t k_pipes = {.name = "pipes",
                                             .ctx_size = sizeof(pipes_state_t),
                                             .reset = pipes_reset,
                                             .render = pipes_render};

static const screensaver_effect_t *const s_registry[] = {
    &k_matrix, &k_plasma, &k_sine, &k_stars, &k_rain, &k_gol, &k_fire, &k_pipes,
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
