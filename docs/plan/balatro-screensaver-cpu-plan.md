# Balatro screensaver CPU-friendly implementation plan

## Goal

Use the Balatro background shader as the visual reference for Home screensaver, but land it on the current ESP32-S3 + LVGL software-rendered path without blocking touch exit, approval overlay, or the rest of the UI thread.

The original source is saved at `docs/plan/balatro-original.glsl`.

## Hardware context

- MCU: ESP32-S3R8, dual-core Xtensa LX7 @ 240 MHz
- Memory: 512 KB SRAM + 8 MB PSRAM (octal SPI, ~40 MB/s effective bandwidth)
- Display: 640 x 172 landscape (172 x 640 physical panel, software rotated)
- LVGL runs single-threaded on one core; the other core handles Wi-Fi and background services
- No GPU; all pixel operations are CPU software rendering

## Current baseline in this repo

Today the screensaver already uses a lightweight offscreen render path inside `src/apps/app_home/src/app_home.c`:

- `HOME_SCREENSAVER_FX_W = 160`, `HOME_SCREENSAVER_FX_H = 43`
- `LV_COLOR_FORMAT_ARGB8888` canvas buffer (27,520 bytes)
- `lv_image_set_scale(1024)` where LVGL uses 256 = 1x, so this is exactly 4x
- `160 * 4 = 640` and `43 * 4 = 172`, so the canvas fills the screen with no letterboxing
- `HOME_SCREENSAVER_FX_PERIOD_MS = 33`, so the background updates at about 30 fps
- all rendering happens on the LVGL task, inside the same screensaver overlay used by the clock label
- current renderer uses `lv_trigo_sin()` which is a 91-entry LUT internally, returning Q15 fixed-point

This is already the right skeleton for a CPU-friendly Balatro-inspired renderer. The main work is changing the math inside `render_screensaver_background()`, not rebuilding the overlay architecture.

## Why the shader cannot be ported directly

The original shader does several things that are cheap on a GPU and expensive on this firmware path:

1. per-pixel floating-point `atan`, `sin`, `cos`, `length`, `abs`, `min`, `max`
2. a five-iteration feedback loop in the hot path
3. a full-screen fragment pipeline that assumes massively parallel execution
4. no concern for the LVGL UI thread budget

At `160 x 43`, each frame is only 6,880 pixels, which is good. The problem is the math density, not the pixel count. A direct C port of the shader would spend too much time inside trigonometry and iterative flow updates, and screensaver exit latency would get worse.

### Shader structure breakdown

The original shader has five distinct stages:

1. **Pixelation** (lines 19-23): Quantizes screen coordinates to create the chunky retro look. This is cheap and we already have it via the low-res canvas.

2. **Polar spin warp** (lines 25-35): Computes `atan2(y, x)` for each pixel, adds a constant offset plus a radius-dependent swirl, then reconstructs Cartesian coords with `cos/sin`. This is the most expensive stage per-pixel.

3. **Scale and time setup** (lines 37-39): Multiplies UV by 30 and sets up time-based speed. Trivial.

4. **Flow distortion loop** (lines 41-47): Five iterations of feedback distortion using `sin/cos`. Each iteration does 4 trig calls and several multiplications. At 5 iterations, this is 20 trig calls per pixel.

5. **Palette mapping** (lines 49-59): Maps the distorted length to a three-color palette with highlights. Uses `length()`, `abs()`, `min()`, `max()`. Cheap once you have the UV.

### Cost analysis for direct port

At 6,880 pixels per frame:

| Stage | Ops per pixel | Total ops | Float cost |
|-------|---------------|-----------|------------|
| Polar spin | 1 atan2, 2 sin/cos | 20,640 trig | Very high |
| Flow loop (5 iter) | 20 sin/cos | 137,600 trig | Dominant |
| Palette | 1 sqrt (length) | 6,880 sqrt | Moderate |

A single `sinf()` on ESP32-S3 without hardware FPU assist costs roughly 50-100 cycles. The flow loop alone would be 7-14 million cycles per frame, which is 29-58 ms at 240 MHz. At 30 fps (33 ms budget), that already consumes nearly the entire frame before counting anything else.

The LVGL task also needs time for compositing, invalidation, and the time label. A direct port would cause visible stutter on touch exit.

## Recommended direction

Use a **runtime procedural approximation** as the primary path, and keep an **offline sprite-sheet fallback** in reserve if visual fidelity matters more than procedural flexibility.

### Primary path: Balatro-lite procedural runtime

Keep the current low-resolution canvas, but replace the floating shader with a fixed-point and LUT-driven approximation.

#### Target visual features to preserve

The original effect has four characteristics worth keeping:

1. **Coarse pixelated sampling** — already achieved by the 160x43 canvas
2. **Radial spin and swirl** — the distinctive "paint being stirred" look
3. **Flowing pigment-like distortion** — organic, time-varying turbulence
4. **Three-color paint blend with highlight bloom** — red, blue, dark teal palette

If those four survive, it will read as "Balatro-like" on the device even if the math is no longer a literal port.

#### CPU-friendly render model

The key insight is that most of the visual character comes from the *flow distortion*, not the polar warp. We can simplify the polar stage heavily and keep more iterations in the flow stage.

For each low-res pixel:

1. Start from precomputed polar coordinates (angle bin and radius) for the 160 x 43 canvas
2. Apply a cheap polar offset using a single LUT lookup per pixel (skip per-pixel atan2)
3. Run a reduced flow loop (2 iterations, not 5) using LUT-based sin/cos
4. Map the result through the original three-color palette and a cheap highlight term

#### Fixed-point format selection

The shader UV coordinates range roughly `[-0.5, 0.5]` after normalization, then get scaled by 30 to `[-15, 15]`, and the flow loop can push values to `[-50, 50]` or beyond.

Recommended format: **Q8.8** (signed 16-bit with 8 fractional bits)

- Range: `[-128, 127.996]` — sufficient for flow loop values
- Precision: `1/256 ≈ 0.004` — adequate for visual output
- Operations: fits in `int32_t` for multiplication without overflow
- Matches well with 8-bit color output

For the sin/cos LUT, use Q1.15 output (like LVGL's `lv_trigo_sin` already does).

#### Sin/cos LUT design

Use the existing `lv_trigo_sin()` function directly — it is already a 91-entry LUT with linear interpolation, returning Q15 results. No need to add another table.

For angle conversion: map the Q8.8 "virtual angle" to degrees (0-359) using a simple modulo and scale.

#### Precomputed per-pixel data

Allocate a small table at screensaver init (once, not per-frame):

```c
typedef struct {
    int16_t x_norm;    // Q8.8, normalized x in [-0.5, 0.5]
    int16_t y_norm;    // Q8.8, normalized y in [-0.5, 0.5]
    int16_t radius;    // Q8.8, sqrt(x² + y²), precomputed with float at init
    int16_t angle_deg; // 0-359, precomputed atan2 at init
} pixel_polar_t;

// 160 * 43 * 8 bytes = 54,720 bytes — fits comfortably in PSRAM
```

This moves all `atan2` and `sqrt` calls out of the render loop entirely.

#### Proposed runtime constants

Start with these conservative defaults:

| Parameter | Value | Rationale |
|-----------|-------|-----------|
| Render size | 160 x 43 | Matches current baseline |
| Update period | 33 ms (30 fps) | Matches current baseline |
| Flow iterations | 2 | Balances visual quality vs cost |
| Angle resolution | Use LVGL's 360° | No extra table needed |
| Spin phase step | 2° per frame | Smooth rotation at 30 fps (~60°/s) |
| Palette phase | Separate, slower | Adds depth without extra math |

#### Estimated cost for optimized path

| Stage | Ops per pixel | Notes |
|-------|---------------|-------|
| Polar offset | 1 LUT read + 2 adds | Angle from precomputed table |
| Flow (2 iter) | 8 LUT reads + 16 mul/add | Down from 20 trig calls |
| Palette | 6 mul/add + 3 clamp | No sqrt needed, use Q8.8 length² |

Per-pixel estimate: ~50-80 cycles (compared to 2000+ for direct port).
Per-frame estimate: ~500,000 cycles = ~2 ms at 240 MHz.

This leaves plenty of headroom for LVGL compositing, time label, and touch response at 30 fps.

#### Graceful degradation path

If frame cost is still too high on hardware, degrade in this order:

1. Drop flow iterations from 2 to 1
2. Slow the timer from 33 ms to 50 ms (20 fps)
3. Simplify palette to 2 colors instead of 3
4. Only then consider lowering render resolution (e.g., 80 x 22 with 8x scale)

This keeps the clean 4x scale factor for the current 640 x 172 layout as long as possible.

## Secondary path: offline sprite-sheet fallback

If the procedural approximation still feels too far from the original, generate frames offline from the saved shader and play them back as a looping low-res animation.

### Storage cost analysis

At 160 x 43:

| Format | Bytes/frame | 24 frames | 32 frames | Notes |
|--------|-------------|-----------|-----------|-------|
| RGB565 | 13,760 | 330 KB | 440 KB | Standard, slight banding |
| ARGB8888 | 27,520 | 660 KB | 880 KB | Matches current canvas |
| RGB888 | 20,640 | 495 KB | 660 KB | Good balance |

For flash budget, RGB565 at 24-32 frames (330-440 KB) is acceptable. The current sprite assets for Notchi already use 384 KB, so this is comparable.

### Generation workflow

1. Run the original shader in a local GLSL sandbox (e.g., Shadertoy export, or a simple OpenGL app)
2. Capture frames at 160 x 43 resolution, 8 fps, for 3-4 seconds (24-32 frames)
3. Convert to C arrays using the existing `tools/sprite_convert.py` or similar
4. Loop playback with index increment in the timer callback

### Tradeoffs vs procedural

| Aspect | Procedural | Sprite-sheet |
|--------|------------|--------------|
| CPU cost | ~2 ms/frame | ~0.1 ms/frame |
| Flash cost | ~55 KB (LUT + precomputed) | ~330-440 KB |
| Dynamic timing | Yes (phase varies) | No (fixed loop) |
| Color tweaking | Easy at runtime | Requires regeneration |
| Visual fidelity | Approximation | Pixel-perfect |

## Recommended architecture for future code work

Do not keep expanding `app_home.c` with more math. Split the new renderer into a small internal module:

- `src/apps/app_home/src/screensaver_balatro.c`
- `src/apps/app_home/src/screensaver_balatro.h`

### Interface contract

```c
// screensaver_balatro.h

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

// Initialize precomputed tables. Call once at app_home init.
// Returns false if PSRAM allocation fails.
bool balatro_init(uint16_t width, uint16_t height);

// Render one frame into the canvas buffer.
// phase_deg: 0-359, global animation phase
// palette_phase: 0-359, slower secondary phase for color variation
void balatro_render(lv_color32_t *pixels, uint32_t stride_px,
                    uint16_t phase_deg, uint16_t palette_phase);

// Free precomputed tables. Call at app_home deinit.
void balatro_deinit(void);
```

### Responsibilities

`app_home.c` should continue to own:

- screensaver lifecycle (enter/exit/touch handling)
- LVGL object creation (canvas, image, overlay)
- timer start and stop
- fallback behavior if buffer allocation fails
- calling `balatro_render()` from the timer callback

The new module should own:

- precomputed polar table allocation and initialization
- frame rendering into the canvas buffer
- all fixed-point math and LUT usage
- tuning constants
- optional debug counters for frame cost measurement

## Concrete implementation steps

1. **Save original shader** (done) — `docs/plan/balatro-original.glsl`

2. **Add renderer module skeleton**
   - Create `screensaver_balatro.c/.h` with the interface above
   - Add to `CMakeLists.txt`
   - Stub out functions with empty bodies

3. **Implement precomputation**
   - Allocate `pixel_polar_t` table in PSRAM
   - Compute normalized coords, radius, and angle for each pixel using float at init time
   - Store as Q8.8 fixed-point

4. **Implement simplified render loop**
   - Port the polar offset (single LUT lookup per pixel)
   - Port 2 iterations of the flow loop using `lv_trigo_sin()`
   - Port the 3-color palette blend

5. **Integrate with app_home**
   - Call `balatro_init()` from `create_screensaver_overlay()`
   - Replace `render_screensaver_background()` body with `balatro_render()` call
   - Keep existing timer and phase management
   - Add compile-time switch to restore old gradient if needed

6. **Measure and tune on device**
   - Add `esp_timer_get_time()` instrumentation around render call
   - Log frame time to serial
   - Adjust iterations/period if needed

7. **Visual tuning**
   - Compare with shader reference on desktop
   - Adjust palette constants if colors feel off on the actual panel
   - Consider gamma correction for the specific LCD

## Validation plan

When implementation starts, validate in this order:

1. **Touch responsiveness** — screensaver still enters and exits instantly on touch (< 50 ms latency)
2. **Approval overlay** — approval overlay still interrupts screensaver correctly
3. **Frame cadence** — the new renderer keeps a stable visual cadence on real hardware (no visible stutter)
4. **Memory stability** — PSRAM allocation and frame updates stay reliable after long idle periods (> 1 hour)
5. **Visual quality** — color balance still reads close to the Balatro reference on the actual panel

### Acceptance criteria

| Metric | Target | Measurement method |
|--------|--------|-------------------|
| Frame render time | < 15 ms | `esp_timer_get_time()` delta |
| Touch exit latency | < 100 ms | Visual observation |
| PSRAM usage | < 100 KB | `heap_caps_get_info()` |
| Flash usage (code) | < 5 KB | Section size in map file |

## Risks

- **Angle warp quality**: The polar offset is the most expensive part; a bad approximation will either look flat (too simple) or get too costly (too accurate). Start with 2 flow iterations and tune from there.

- **Fixed-point overflow**: Q8.8 multiplication can overflow in `int16_t`. All multiplications must use `int32_t` intermediate and shift back. The render loop must be careful about this.

- **PSRAM latency**: The precomputed table is in PSRAM. Random access patterns may hit cache misses. If this becomes an issue, consider row-major iteration order or smaller per-row tables.

- **Scope creep**: If the procedural version chases pixel-perfect fidelity, it will lose the CPU budget battle. Accept "visually similar" as the goal, not "mathematically identical."

- **LVGL version changes**: The plan assumes `lv_trigo_sin()` continues to exist. If LVGL updates break this, we may need to inline a sin LUT.

## Decision

The recommended plan is:

1. **Primary:** build a fixed-point, LUT-driven Balatro-lite runtime renderer on the existing 160 x 43 canvas, targeting < 15 ms per frame with 2 flow iterations.

2. **Fallback:** if visual quality is not good enough, switch to an offline sprite-sheet generated from the saved shader (24 frames, RGB565, ~330 KB flash).

3. **Emergency fallback:** if both fail, keep the existing four-corner gradient as a compile-time option.

That gives the project the best balance between CPU cost, code ownership, and visual character.

## Appendix: key shader constants

For reference when tuning the CPU version:

```c
// Original Balatro palette (0-255 range)
#define BALATRO_COLOR1_R 222  // 0.871 * 255
#define BALATRO_COLOR1_G  68  // 0.267 * 255
#define BALATRO_COLOR1_B  59  // 0.231 * 255

#define BALATRO_COLOR2_R   0  // 0.0 * 255
#define BALATRO_COLOR2_G 107  // 0.42 * 255
#define BALATRO_COLOR2_B 180  // 0.706 * 255

#define BALATRO_COLOR3_R  22  // 0.086 * 255
#define BALATRO_COLOR3_G  35  // 0.137 * 255
#define BALATRO_COLOR3_B  37  // 0.145 * 255

// Contrast and lighting
#define BALATRO_CONTRAST    3.5f   // affects color band width
#define BALATRO_LIGHTING    0.4f   // highlight intensity
#define BALATRO_SPIN_AMOUNT 0.25f  // swirl strength

// Timing
#define BALATRO_SPIN_SPEED  7.0f   // flow animation rate
```
