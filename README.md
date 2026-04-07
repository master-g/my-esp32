# ESP32 Dashboard

This repository tracks the design and host-side tooling for an ESP32-S3 dashboard project.

## Repository Layout

- `docs/`
  Project design documents, architecture notes, and sub-designs.
- `tools/claude_bridge/`
  Rust daemon and CLI that receive Claude Code hook events on the host machine and expose device-facing WebSocket state.

## Intentionally Not Tracked

The following local directories are kept out of version control on purpose:

- `test-blink/`
  Temporary board validation example project.
- `.claude/`, `.omc/`, `build/`, `target/`
  Local assistant state and generated build artifacts.

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

- [docs/2025-04-06-esp32-dashboard-design.md](./docs/2025-04-06-esp32-dashboard-design.md)
- [docs/2026-04-07-claude-app-subdesign.md](./docs/2026-04-07-claude-app-subdesign.md)

## Next Steps

- Add the embedded firmware project structure when implementation starts.
- Wire the ESP32 client to the `claude_bridge` WebSocket protocol.
