# ESP32 Dashboard

This repository now tracks both the host-side tooling and the initial ESP-IDF firmware scaffold for an ESP32-S3 dashboard project.

## Repository Layout

- `src/`
  Firmware source tree root.
- `docs/`
  Project design documents, architecture notes, setup guides, hardware baselines, and plans.
- `src/main/`
  ESP-IDF firmware bootstrap entrypoint.
- `src/components/`
  Firmware core modules, BSP contracts, services, and policies.
- `src/apps/`
  Four fixed app slots: `Home`, `Notify`, `Trading`, `Satoshi Slot`.
- `tools/esp32dash/`
  Rust host companion CLI and local agent for Claude hook ingestion, USB serial config management, and device control.

## Intentionally Not Tracked

The following local directories are kept out of version control on purpose:

- `.vscode/`
  Machine-local ESP-IDF extension state, including absolute toolchain paths.
- `.claude/`, `.omc/`, `build/`, `target/`
  Local assistant state and generated build artifacts.
- `managed_components/`, `sdkconfig`, `sdkconfig.old`
  Generated ESP-IDF dependency and local configuration outputs.

ESP-IDF itself is expected to be installed and managed outside this repository, for example through Espressif Install Manager and the VS Code ESP-IDF extension.

See [`.gitignore`](./.gitignore) for the full ignore policy.

## esp32dash

`tools/esp32dash/` is a standalone Rust project.

Common commands:

```bash
cd tools/esp32dash
cargo run -- agent run
cargo run -- agent status
```

The bridge is documented in:

- [docs/design/dashboard-design.md](./docs/design/dashboard-design.md)
- [docs/design/apps/claude-app.md](./docs/design/apps/claude-app.md)

## Firmware Baseline

The initial firmware scaffold follows the official Waveshare ESP-IDF examples for the board-level baseline:

- `AXS15231B` QSPI LCD
- physical panel `172 x 640` portrait layout
- runtime UI `640 x 172` landscape layout
- dual I2C buses
  - RTC + IMU on `GPIO48/GPIO47`
  - touch on `GPIO18/GPIO17`
- device addresses
  - touch `0x3B`
  - RTC `0x51`
  - IMU `0x6B`

The rationale and the source discrepancy notes are documented in [docs/hardware/waveshare-board-baseline.md](./docs/hardware/waveshare-board-baseline.md).

## Firmware Commands

The repository ships a small `Makefile` wrapper around `idf.py`.

Common commands:

```bash
make build
make ports
make port
make flash
make monitor
make flash-monitor
```

By default `PORT=auto`, which means `make flash` and `make monitor` will try to auto-detect the active serial device. If multiple candidates exist, pass the port explicitly:

```bash
make flash PORT=/dev/cu.usbmodem101
make monitor PORT=/dev/cu.usbmodem101
```

The flash wrapper also does a small preflight check and will fail early if the selected serial device is already busy. If you intentionally want to skip that check, set `PORT_BUSY_OK=1`.

## Current State

The firmware side currently provides:

- ESP-IDF project entrypoints
- a real `bsp_display` bring-up path using `esp_lcd_axs15231b + LVGL`
- real board access paths for touch and RTC over I2C
- a stable software-rotated landscape baseline using the official Waveshare LVGL path
- two fixed landscape orientations, with the current default set to the flipped landscape direction
- app manager and event bus runtime wiring
- power policy core logic
- Wi-Fi credential load + reconnect flow in `net_manager`
- RTC restore + SNTP sync in `service_time`
- a `home_service` aggregation layer consumed by `app_home`
- four registered app slots with LVGL roots
- board configuration constants pinned to the Waveshare examples
- first-round device validation for `app_home`

What is intentionally not wired yet:

- real weather / market transports, plus the firmware-side `esp32dash` serial transport
- production-ready `Notify` / `Trading` / `Satoshi Slot` UI
- end-to-end backlight / dim / sleep control validation
- IMU-backed auto rotation / auto wake behavior
