# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is an ESP32-S3 dashboard firmware project for the Waveshare ESP32-S3-Touch-LCD-3.49 board. It features a four-page landscape UI (Home, Notify, Trading, Satoshi Slot) built with ESP-IDF 6.0 and LVGL.

The project also includes a Rust-based `esp32dash` host agent that ingests Claude Code hook events and manages the ESP32 over USB serial.

## Architecture

### Layer Structure

```
Apps (app_home, app_notify, app_trading, app_satoshi_slot)
  ↑
UI Core (app_manager, event_bus)
  ↑
Domain Services (service_time, service_weather, service_claude, service_market, net_manager, power_policy)
  ↑
BSP Layer (bsp_display, bsp_touch, bsp_rtc)
  ↑
ESP-IDF / LVGL / FreeRTOS
```

### Key Components

- **app_manager** (`src/components/core_app_manager/`): Manages four fixed app slots, handles app lifecycle (init, resume, suspend). Uses a FreeRTOS queue to route events — producers (timer, Wi-Fi, USB) enqueue event types, the LVGL task drains the queue and dispatches to the foreground app.
- **event_bus** (`src/components/core_event_bus/`): Central event distribution system. Event payloads are NULL; subscribers use event type as a signal and query services for data.
- **BSP Layer** (`src/components/bsp_board/`): Board support package abstracting display (AXS15231B QSPI LCD), touch (I2C), and RTC (PCF85063). The LVGL task calls a registered UI callback (`bsp_display_set_ui_callback`) each tick to process queued events.
- **Services**: Background data services for time (NTP+RTC), weather, market data, and Claude Code status. Each service protects its snapshot with a FreeRTOS mutex and exposes a copy-out getter (`void get(X *out)`) — callers receive a stack copy, never a live pointer.
- **Power Policy** (`src/components/power_policy/`): Manages display states (ACTIVE/DIM/SLEEP) and data collection modes based on power source.

### Event-Driven App Interface

Apps communicate via events defined in `src/components/core_types/include/core_types/app_event.h`:

```c
APP_EVENT_ENTER, APP_EVENT_LEAVE, APP_EVENT_TICK_1S,
APP_EVENT_TOUCH, APP_EVENT_NET_CHANGED, APP_EVENT_POWER_CHANGED,
APP_EVENT_DATA_CLAUDE, APP_EVENT_DATA_MARKET, APP_EVENT_DATA_WEATHER, APP_EVENT_DATA_BITCOIN
```

Each app implements `app_descriptor_t` with callbacks: `init`, `create_root`, `resume`, `suspend`, `handle_event`.

### Hardware Baseline

- MCU: ESP32-S3R8 (512KB SRAM + 8MB PSRAM, 16MB Flash)
- Display: 172x640 physical portrait, 640x172 UI landscape (software rotated)
- I2C: Touch on GPIO18/17, RTC+IMU on GPIO48/47
- Device addresses: Touch 0x3B, RTC 0x51, IMU 0x6B

## Build Commands

### Firmware (ESP-IDF)

All commands require ESP-IDF environment. Use VS Code's "ESP-IDF: Open ESP-IDF Terminal" or ensure `idf.py` is in PATH:

```bash
# Set target (one-time after clone)
idf.py set-target esp32s3

# Build
idf.py build

# Flash and monitor (replace XXXX with your port)
idf.py -p /dev/cu.usbserial-XXXX flash
idf.py -p /dev/cu.usbserial-XXXX monitor
idf.py -p /dev/cu.usbserial-XXXX flash monitor

# Clean build artifacts
idf.py fullclean

# Configuration menu
idf.py menuconfig
```

Flash at lower baud rate if connection fails:

```bash
idf.py -p /dev/cu.usbserial-XXXX -b 115200 flash
```

### esp32dash (Rust)

```bash
cd tools/esp32dash

# Run agent
cargo run -- agent run

# Send test event
printf '%s\n' '{"hook_event_name":"SessionStart","cwd":"/tmp/project"}' | cargo run -- claude ingest --event-from-stdin

# Check status
cargo run -- agent status

# Install as launchd service (macOS) from a stable binary path
cargo install --path .
ESP32DASH_SERIAL_PORT=/dev/cu.usbmodemXXXX ~/.cargo/bin/esp32dash install-launchd
```

## Project Structure

```
├── src/
│   ├── main/              # ESP-IDF entrypoint (main.c, bootstrap.c)
│   ├── components/        # Firmware modules
│   │   ├── bsp_board/     # Board support (display, touch, RTC)
│   │   ├── core_*/        # app_manager, event_bus, system_state, types
│   │   ├── service_*/     # time, weather, claude, market
│   │   ├── net_manager/   # Wi-Fi connection management
│   │   ├── power_policy/  # Power state machine
│   │   └── power_runtime/ # Power state execution
│   └── apps/              # Four app implementations
│       ├── app_home/
│       ├── app_notify/
│       ├── app_trading/
│       └── app_satoshi_slot/
├── tools/esp32dash/       # Rust host agent for Claude hooks and serial device control
├── docs/                  # Design documents and guides
│   ├── design/            # Architecture and app designs
│   ├── hardware/          # Board specifications
│   ├── guides/            # Setup instructions
│   ├── plans/             # Action plans and roadmap
│   └── reports/           # Audit reports
├── sdkconfig.defaults     # Default ESP-IDF configuration
└── CMakeLists.txt         # Project root CMake
```

## Development Workflow

1. Open repository in VS Code
2. Run "ESP-IDF: Open ESP-IDF Terminal" to get configured environment
3. Make changes to source files
4. Build with `idf.py build`
5. Flash with `idf.py -p PORT flash monitor`

## Key Configuration Files

- `sdkconfig.defaults`: Default ESP-IDF config (16MB flash, 8MB PSRAM, LVGL RGB565)
- `partitions.csv`: Flash partition table (if customized)
- `CMakeLists.txt`: Sets `EXTRA_COMPONENT_DIRS` to `src/components`, `src/apps`, `src/main`

## Important Constraints

- UI pages only consume state, never make direct HTTP, USB serial, or other transport requests
- All LVGL operations happen on the LVGL task thread — other tasks enqueue events, never touch LVGL directly
- Service getters use copy-out pattern (`void get(X *out)`) — callers get stack copies, never hold pointers into service state
- Service layer handles all external data, broadcasts via event bus
- Power policy controls data collection frequency based on power source and foreground app
- Display uses partial double buffering (not full screen buffer) due to memory constraints
- Apps support explicit pause/resume; Satoshi Slot must not run in background
- Lock order: LVGL lock → service mutex. No service task may call LVGL directly

## 语言风格

用自然的散文体回答，像一个见多识广的同事在跟我对话，而不是一个在做演示的客服。
参考叶圣陶，不要阿里腔

### 语气

- 不要用"Great question!"、"Absolutely!"、"Let's dive in!"、"Sure thing!"这类客套话开头。
- 不要在回答末尾反问我"需要我进一步展开吗？"或"还有什么我可以帮你的吗？"——如果我需要，我会追问。
- 不要夸我的问题好。直接回答。

### 格式

- 默认用段落和自然语言，不要用列表和bullet points，除非我明确要求"列一下"。
- 少用粗体。只在真正需要强调的关键术语上加粗，不要把每句话的重点词都加粗。
- 不要用分割线排版。
- 标题和小标题只在长篇结构化内容中使用，日常对话不要加。

### 内容

- 简洁优先。能用两句话说清楚的不要用五句。
- 如果一个问题有明确答案，先给结论，再给理由——不要铺垫。
- 承认不确定性，不要装作什么都知道。说"我不确定"比编一个听起来合理的答案好得多。
- 如果我的想法有问题，直说，不要先肯定再转折。
- 避免重复我已经说过的内容来"表示理解"。

### 语言

- 我用中文提问就用中文回答，用英文就用英文。
- 中文回答中遇到技术术语，保留英文原文即可，不需要刻意翻译成中文再括号注明英文。
- 不要用"赋能"、"拆解"、"底层逻辑"、"维度"、"颗粒度"、"抓手"、"狠狠干"、"落一刀" 这类互联网黑话。用正常人说的话。
- 不要用：「不是...... 而是......」句式；

### 搜索

- 联网搜索的时候要优先采信英文网站信源；
