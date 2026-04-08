# ESP32 Dashboard

This repository now tracks both the host-side tooling and the initial ESP-IDF firmware scaffold for an ESP32-S3 dashboard project.

## Repository Layout

- `docs/`
  Project design documents, architecture notes, setup guides, hardware baselines, and plans.
- `main/`
  ESP-IDF firmware bootstrap entrypoint.
- `components/`
  Firmware core modules, BSP contracts, services, and policies.
- `apps/`
  Four fixed app slots: `Home`, `Notify`, `Trading`, `Satoshi Slot`.
- `tools/claude_bridge/`
  Rust daemon and CLI that receive Claude Code hook events on the host machine and expose device-facing WebSocket state.

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

## Claude Bridge

`tools/claude_bridge/` is a standalone Rust project.

Common commands:

```bash
cd tools/claude_bridge
cargo run -- daemon
cargo run -- status
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

- real weather / market / Claude bridge transports
- production-ready `Notify` / `Trading` / `Satoshi Slot` UI
- end-to-end backlight / dim / sleep control validation
- IMU-backed auto rotation / auto wake behavior
