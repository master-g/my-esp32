# Home screensaver optimization plan

> Note: The active Home screensaver background now lives in `src/apps/app_home/src/screensaver_renderer.c` and follows the newer `screensaver.glsl`-style effect. This document remains useful as historical context for the direct-mode pipeline and earlier Balatro-specific measurements.

## Goal

Keep the Balatro shader as the visual reference, but optimize the **current direct-to-panel screensaver path** instead of returning to the old LVGL full-render path.

The original shader source is kept at `docs/plan/balatro-original.glsl`.

This document supersedes the earlier assumption that the first priority should be “make the Balatro math cheaper on the LVGL canvas”. The measured bottleneck has changed.

## What we know now

### Hardware and threading baseline

- MCU: ESP32-S3R8, **dual-core** Xtensa LX7 @ 240 MHz
- Memory: 512 KB SRAM + 8 MB PSRAM
- Physical panel: `172 x 640` portrait
- Runtime UI: `640 x 172` landscape
- FreeRTOS is **dual-core, non-SMP**
- `lvgl_port_task` is pinned to **core 0**
- `time_service`, `weather_service`, `device_link`, and `power_runtime` are pinned to **core 1**

This means the firmware already uses both cores, but the **screensaver frame loop itself is still serialized on the LVGL side**. The current `screensaver_fx_timer_cb()` runs on the LVGL thread, so screensaver rendering does not yet exploit frame-level parallelism across two cores.

### Measured bottleneck progression

Three measurements matter:

1. **Balatro on old LVGL path**
   - about `2.2 fps`
   - about `56 ms` renderer time

2. **Mock renderer on old LVGL path**
   - about `2.4 fps`
   - about `2.3 ms` renderer time

3. **Mock renderer on direct path**
   - about `14 fps`
   - about `60.5 ms` displayed “CPU” time

The second number proved that the old bottleneck was not primarily Balatro math; it was the LVGL full-frame render / rotate / flush path.

The third number proved that the **direct path is the right direction**. Once screensaver stopped going through LVGL full-frame composition, frame rate jumped from roughly `2.4 fps` to `14 fps`.

### Important metric caveat

The current top-left `CPU` metric in direct mode is **not pure renderer CPU time**.

It measures the wall time around `render_screensaver_background()`, which currently includes:

- direct background composition
- text blit
- `bsp_display_push_native_rgb565()`
- blocking panel push / wait time

So the current `60.5 ms` should be read as **“direct screensaver frame wall time”**, not “Balatro math cost”.

## Current implementation state

### What is already done

- `bsp_display` now exposes a screensaver-only direct mode:
  - `bsp_display_begin_direct_mode()`
  - `bsp_display_end_direct_mode()`
  - `bsp_display_push_native_rgb565()`
- `app_home` already switches panel ownership when screensaver enters/exits
- `screensaver_direct.c` renders directly into native RGB565 and pushes to the panel
- direct-mode mock path is hardware-validated enough to produce the `14 fps` result
- `screensaver_balatro.c` still exists as the Balatro CPU approximation module

### What is not true anymore

The active screensaver path is **not** currently the Balatro renderer.

At the moment, the real on-device test path is the **direct mock compositor** in `screensaver_direct.c`. That is intentional: it isolates the new display path from shader complexity.

### What Balatro already optimizes

The existing Balatro CPU module is not a naive port. It already includes:

- precomputed polar table
- Q8.8 fixed-point math in the hot loop
- `lv_trigo_sin()` LUT-based runtime trig
- reduced flow loop count
- runtime `length()` approximation with Manhattan-distance proxy instead of real `sqrt`

So if Balatro becomes the next bottleneck later, the work is no longer “start optimizing from zero”. The work becomes “fit the existing Balatro core into a cheaper direct compositor”.

## Main conclusion

The next optimization target is **the direct compositor itself**, not `hardware rotation`, not global `PARTIAL` refactor, and not a premature second-core rewrite.

The current mock direct path still does three expensive things:

1. it shades the full logical `640 x 172` output, not a cheap low-resolution background
2. it maps logical coordinates to native panel coordinates **pixel by pixel**
3. it redraws the whole frame, including text, every tick

That is why frame rate improved dramatically, but wall time is still high.

## Recommended optimization order

### Phase 1: split the direct-path timing

Before any further rewrite, split the direct-mode timing into separate metrics.

Minimum required counters:

- `compose_us` — background fill only
- `text_us` — time / debug glyph blit
- `push_us` — memcpy + panel draw issue time
- `wait_us` — blocking wait for flush completion
- `frame_us` — end-to-end wall time

Without this split, later tuning will keep mixing compositor cost with panel-transfer cost.

### Phase 2: make the direct compositor cheap

This is the highest-value next step.

#### 2.1 Background should return to low resolution

Do not keep generating a full-resolution procedural background.

Recommended first target:

- internal background size: `160 x 43`
- optional fallback: `128 x 32` if needed

Then upscale into the final output.

The key point is not just “fewer pixels”; it is also fewer coordinate transforms and less per-pixel math.

#### 2.2 Stop using per-pixel logical-to-native mapping in the hot loop

The current direct path calls a logical-to-native transform for every pixel write. That is convenient, but it is not cheap.

The next version should render in **native row order** or at least use a **scanline helper** so the hot loop becomes linear memory writes.

In practice, this means:

- background generation should write rows in panel-native order
- 4x upscale should happen while walking rows, not by calling `put_pixel_logical()` repeatedly
- logical-landscape math can remain at the control level, but not in the inner pixel loop

#### 2.3 Text should stop repainting from scratch every frame

The time string changes roughly once per second. FPS/debug text also changes much slower than the frame cadence we want.

So the next direct compositor should:

- cache the rendered time overlay
- only redraw time when `HH:MM` changes
- only redraw debug overlay when the displayed numbers change

Static overlay reuse will cut pointless per-frame work.

### Phase 3: put Balatro back on top of the cheaper direct path

Only after Phase 2 lands should the real Balatro renderer be reintroduced as the active background.

Recommended rule:

1. first optimize the direct compositor with the mock path
2. then swap the background generator from mock to Balatro
3. then re-measure

If Balatro becomes the new limiter, degrade in this order:

1. reduce flow iterations
2. reduce internal render resolution
3. simplify palette / lighting
4. only then consider offline sprite-sheet fallback

### Phase 4: use the second core only if the cheaper compositor still is not enough

The chip is dual-core, but the current screensaver does not yet gain much from that fact.

The correct two-core optimization is not “move one function to another task” casually. It is a proper frame pipeline:

- **core 1** prepares the next frame in a back buffer
- **core 0** owns panel push for the current frame
- a small queue or double-buffer handshake swaps the buffers safely

This should be attempted only after the compositor has been made cheap enough to justify the added complexity.

Otherwise, two cores will just be used to parallelize inefficient work.

## What should stay low priority

### Hardware rotation spike

Keep it low priority.

The external evidence around AXS15231B rotation support is still weak enough that it should remain a small experiment, not the mainline plan.

### Global LVGL PARTIAL refactor

Keep it lower priority than screensaver-specific work.

That refactor may still be useful for the overall UI, but it is no longer the best next move for screensaver performance.

## Concrete next steps

1. Add split timing to `screensaver_direct.c` and the on-screen debug overlay.
2. Replace full-resolution mock background with low-resolution background + row-wise upscale.
3. Remove pixel-by-pixel logical coordinate mapping from the hot loop.
4. Cache rendered time/debug overlays instead of repainting them every frame.
5. Re-measure mock direct path on hardware.
6. If mock direct path is comfortably faster, switch the active background generator back to Balatro and measure again.
7. Only if that still is not good enough, prototype a true two-core frame pipeline.

## Acceptance targets

Use staged targets instead of a single number.

### Stage A: optimized mock direct path

- visible cadence clearly above the current `14 fps`
- target: `20 fps+`
- `compose_us` should drop substantially versus the current wall-time figure

### Stage B: Balatro on optimized direct path

- frame rate should remain clearly above the old `2.4 fps` LVGL-path baseline
- motion should remain visually alive, not look static
- touch exit and approval interruption should stay immediate

## Decision

The next version should **not** start with more Balatro math tuning.

The next version should:

1. treat the current `14 fps` result as confirmation that direct mode is correct
2. treat the current `60.5 ms` figure as a compositor-plus-push wall-time problem
3. optimize the direct compositor first
4. reintroduce Balatro only after that cheaper path exists

That is the shortest path from “proof that direct mode works” to a screensaver that is both fast enough and visually worth keeping.
