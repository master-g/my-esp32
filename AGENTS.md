# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is an ESP32-S3 dashboard firmware project for the Waveshare ESP32-S3-Touch-LCD-3.49 board. It features a three-page landscape UI (Home, Trading, Settings) built with ESP-IDF 6.0 and LVGL.

The project also includes a Rust-based `esp32dash` host agent that ingests Claude Code hook events and manages the ESP32 over USB serial.

## Architecture

### Layer Structure

```
Apps (app_home, app_trading, app_settings)
  ↑
UI Core (app_manager, event_bus)
  ↑
Domain Services (service_time, service_weather, service_claude, service_market, home_service, settings_service, net_manager, power_policy)
  ↑
BSP Layer (bsp_display, bsp_touch, bsp_rtc)
  ↑
ESP-IDF / LVGL / FreeRTOS
```

### Key Components

- **app_manager** (`src/components/core_app_manager/`): Manages the fixed app roster, handles app lifecycle (init, resume, suspend). Uses a FreeRTOS queue to route events — producers (timer, Wi-Fi, USB) enqueue event types, the LVGL task drains the queue and dispatches to the foreground app.
- **event_bus** (`src/components/core_event_bus/`): Central event distribution system. Event payloads are NULL; subscribers use event type as a signal and query services for data.
- **device_protocol** (`src/components/device_protocol/`): The hardware-free part of the host/device serial protocol — line assembly with the overflow guard, `claude.update` parsing, and the method list the device declares. Depends only on cJSON and libc; `device_link` is the serial adapter on top of it. `make test-host` builds it with the system `cc` and checks it against `protocol/fixtures/`, the same sample frames the Rust agent's tests read.
- **BSP Layer** (`src/components/bsp_board/`): Board support package abstracting display (AXS15231B QSPI LCD), touch (I2C), and RTC (PCF85063). The LVGL task calls a registered UI callback (`bsp_display_set_ui_callback`) each tick to process queued events.
- **Services**: Background data services for time (NTP+RTC), weather, market data, and Claude Code status. Each service protects its snapshot with a FreeRTOS mutex and exposes a copy-out getter (`void get(X *out)`) — callers receive a stack copy, never a live pointer.
- **Power Policy** (`src/components/power_policy/`): Decides how hard each service may work for the foreground app. The board runs from USB with the screen always on at full brightness; there is no battery, dim or sleep state.

### Event-Driven App Interface

Apps communicate via events defined in `src/components/core_types/include/core_types/app_event.h`:

```c
APP_EVENT_ENTER, APP_EVENT_LEAVE, APP_EVENT_TICK_1S,
APP_EVENT_TOUCH, APP_EVENT_NET_CHANGED, APP_EVENT_POWER_CHANGED,
APP_EVENT_DATA_CLAUDE, APP_EVENT_DATA_MARKET, APP_EVENT_DATA_WEATHER,
APP_EVENT_DATA_SETTINGS
```

Each app implements `app_descriptor_t` with callbacks: `init`, `create_root`, `resume`, `suspend`, `handle_event`.

### Hardware Baseline

- MCU: ESP32-S3R8 (512KB SRAM + 8MB PSRAM, 16MB Flash)
- Display: 172x640 physical portrait, 640x172 UI landscape (software rotated)
- I2C: Touch on GPIO18/17, RTC+IMU on GPIO48/47
- Device addresses: Touch 0x3B, RTC 0x51, IMU 0x6B

## Commands

Everything goes through `make` (`make help` lists the targets); this machine's shell has no `idf.py` on PATH, the Makefile finds ESP-IDF v6.0 itself.

- `make check` — every fast check: Rust format, `cargo test`, firmware host tests against `protocol/fixtures/` (needs `brew install cjson`). CI (`.github/workflows/check.yml`) runs it plus a firmware build on every push.
- `make fmt` — `cargo fmt` plus clang-format on firmware C. Run it **before** `make build`, so what gets flashed is what gets committed.
- `make build` / `make flash` / `make monitor` — firmware. `flash` pauses the launchd agent that holds the serial port and restarts it afterwards.
- `cd tools/esp32dash && cargo install --path .`, then `launchctl kickstart -k gui/$(id -u)/com.local.esp32dash` — put a changed agent into service. The wire protocol has no version negotiation: change host and firmware together.

Looking at the real device (all of these work while the agent is running; they ask it to step off the port):

- `esp32dash device screenshot --out x.png` — what is on the screen now.
- `esp32dash device rpc <method> '<json>'` — any RPC, e.g. `app.switch '{"app":"trading"}'`, `debug.market_snapshot`.
- `esp32dash chibi test --state working --workspace demo --bubble "..." [--pending approval] [--hold 15]` — put a Home state on screen for `--hold` seconds; `chibi screensaver enter --effect stars`.
- `esp32dash agent status` — the shown snapshot, every tracked session (workspace, status, pid, idle time) and `events_received`.

## Important Constraints

- UI pages only consume state, never make direct HTTP, USB serial, or other transport requests
- All LVGL operations happen on the LVGL task thread — other tasks enqueue events, never touch LVGL directly
- Service getters use copy-out pattern (`void get(X *out)`) — callers get stack copies, never hold pointers into service state
- Service layer handles all external data, broadcasts via event bus
- Power policy controls data collection frequency based on the foreground app
- Display renders in `LV_DISPLAY_RENDER_MODE_FULL` with two full-frame PSRAM buffers plus a rotation buffer: every invalidation redraws, rotates and pushes the whole 640x172 frame, so keep Home's animation timers slow
- Apps support explicit pause/resume; Home shows the coarse type of the one pending interaction (tool permission, Elicitation, AskUserQuestion; terms in GLOSSARY.md) as a **read-only** sprite bubble — it never returns a decision
- The ESP32 is a read-only status notifier for Claude authorizations: every decision (allow / deny / allow_always, Elicitation, AskUserQuestion) is made on the Mac's native prompt, so a locked Mac cannot be bypassed from the device. The host decides which pending interaction to show and sends it as a level inside every `claude.update` (`pending`: kind + coarse type, or null); there are no request/dismiss frames and no decision frames or RPCs in either direction (docs/adr/0001). `service_claude` owns that state; Home only renders it. While an interaction is pending the bubble shows only its coarse type — never command, argument, or option content. With nothing pending the bubble shows the snapshot's `detail` (a prompt preview or tool name) next to the session's workspace name
- `protocol.error` and the serial line-overflow guard are unrelated to approvals and stay load-bearing — do not revert them
- Lock order: LVGL lock → service mutex. No service task may call LVGL directly


## 技术栈
- 固件: C + ESP-IDF 6.0，目标 `esp32s3`；UI 用 LVGL 9；运行时 FreeRTOS。依赖见 `src/main/idf_component.yml`。
- 宿主代理 `tools/esp32dash/`: Rust edition 2024。hook 通过本机 HTTP（`127.0.0.1:37125`）把事件交给代理，代理通过 USB 串口的 JSON 行协议驱动设备。

## 代码风格
- C: `.clang-format`，由 pre-commit 的 clang-format hook 强制（`make fmt`）。
- Rust: `tools/esp32dash/.rustfmt.toml`（`cargo fmt`）。

## 禁止文件
- 构建产物: `build/`、`build-*/`、`managed_components/`、`target/`（勿手改）。
- 配置: `sdkconfig`、`sdkconfig.*`、`sdkconfig.old`（生成物）；唯一手改的基线是 `sdkconfig.defaults`。
- 生成代码: `src/apps/*/src/generated/`、`src/components/*/src/generated/`、`tools/lv_font_pipeline/*_tmp.c`（字体/精灵流水线产物，只能由脚本生成）。
- 本地状态: `.claude/`、`.omc/`、`.pre-commit-cache/`、`plans/`。

## 审查规则
- 提交信息遵循 Conventional Commits 带 scope：`feat(...)` / `fix(...)` / `refactor(...)` / `docs(...)`。
- 提交前 `make check` 和 pre-commit 通过；改动固件需 `make build` 通过。

## 工作流与陷阱

<!-- 每条写做法 + 原因。需用户确认才能做的操作（推送、开 PR、跑付费 CI、改生产配置）；慢命令把输出重定向到文件再选读，不用管道过滤；按改动范围选构建 / 测试变体；检出过旧先同步；踩过的坑及其原因。 -->

- 推送、合并、开 PR 等用户发话；本地提交、烧录、`cargo install`、重启 launchd 服务可以直接做。原因：仓库公开，用户要自己把关对外可见的动作。
- 改了界面就在真机上看一眼：`esp32dash chibi test ...` 摆出状态，`esp32dash device screenshot --out x.png` 截图。原因：固件没有 UI 测试，布局问题只有屏幕上看得出来。
- `[emotion]` 保持关闭，也不要建议把提示词发给第三方模型接口。原因：用户不希望自己的提示词被其他供应商看到。
- `cargo install` 覆盖二进制后立刻 `launchctl kickstart -k`，新进程第一次会被系统以 `OS_REASON_CODESIGNING` 杀掉，约 10 秒后 KeepAlive 自动拉起。原因：这是 macOS 对刚替换的二进制的行为，不是损坏，等一下再查状态。
- 看固件日志要接 UART0（115200）那条线；usbmodem 口只有协议帧。设备状态用 `esp32dash device rpc device.info` 查。
- 用户点了授权之后，Claude Code 要等工具跑完才发下一个 hook（PostToolUse），所以长命令期间设备上的提示会一直留着。这是 hook 时序决定的，不要当成 bug 去修。
- Home sprite 改图：改 `tools/sprite_assets/*.png` 后跑 `python3 tools/sprite_convert.py` 重新生成，再 `make test-host`。原因：`generated/sprite_sheets.c` 只能由脚本产出。

## 项目记忆 (回写约定)

跨会话的持久信息记录在 [PROJECT_MEMORY.md](./PROJECT_MEMORY.md)，只写代码、Git 和文档推导不出的内容。

收尾顺序：验证 → 回写记忆 → 提交 → 推送。回写与本次改动进同一个提交；不提交的任务在结束前回写。

回写是提炼，不是记录：

- 「上次会话」「下次运行」整节改写成一块（分支、验证命令与实际结果、停在何处；接下来做什么），旧块直接删，历史由 Git 保存。
- 本次确认的决策追加到「已验证的事实」，走不通的路径及原因追加到「失败尝试」；每条一行，写结论、原因和来源或适用范围。能从 `git log`、代码或计划文档 30 秒内推出来的（commit SHA、进度状态、计划内容）不写，只留一行指向证据目录。
- 同主题已有条目就改写它，不追加；结论被推翻就删旧条。写不进两行的长期知识搬进 docs/ 后只留一行指针；反复出现或每次会话都要先知道的陷阱，晋升到本文件「工作流与陷阱」后从记忆里删掉。
- 结束前核对门槛：全文 ≤ 24 KB，单行 ≤ 300 字符，改写节各一块。装了 bootstrap-claude 技能的会话用它的 `scripts/memory.py check` 校验，`compact` 只压改写节；追加节条目由人合并或搬出，脚本不自动删。

<!-- bootstrap-claude convention v3 -->
