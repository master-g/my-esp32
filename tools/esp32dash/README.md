# esp32dash

`esp32dash` is the host-side companion CLI for the dashboard firmware. It runs a local agent for Claude Code hook ingestion, keeps the latest normalized Claude snapshot, and talks to the ESP32 over USB serial for configuration and management.

`esp32dash config` now drives an interactive setup flow:

- manage multiple Wi-Fi profiles, including visible and hidden SSIDs
- choose a friendly timezone name, which maps to the device's POSIX `TZ` string
- search weather cities through Open-Meteo geocoding, then store `city_label + latitude + longitude`

## Commands

```bash
cargo run -- agent run
cargo run -- agent status
cargo run -- claude ingest --event-from-stdin
cargo run -- device list
cargo run -- device info
cargo run -- device reboot
cargo run -- device screenshot --out screen.png
cargo run -- config
cargo run -- install-launchd
cargo run -- uninstall-launchd
cargo run -- chibi test --state idle --emotion happy --bubble "Nice"
cargo run -- install-hooks
cargo run -- chibi approve-dismiss --delay-ms 1500
cargo run -- chibi screensaver enter
```

`chibi approve-dismiss` prefers the real host-side approval flow. When the local agent is reachable, it posts a synthetic `PermissionRequest`, submits a matching approval, then posts a follow-up `PreToolUse` event so the agent dismisses the ESP32 overlay through the same path used by normal Claude hooks. If the local agent is unavailable, it falls back to a direct serial smoke test and first forces `home.screensaver` off so the overlay is visible.

`chibi test` is the quickest Home UI smoke harness. Besides `--state`, `--emotion`, and `--bubble`, it now also accepts snapshot-level overrides like `--unread auto|on|off`, `--attention auto|low|medium|high`, plus `--title`, `--source`, `--session-id`, and `--event`, so you can exercise unread-dot, notification, and idle-state cases without waiting for real Claude hooks.

## Environment

```bash
export ESP32DASH_ADMIN_ADDR="127.0.0.1:37125"   # optional
export ESP32DASH_SERIAL_PORT="/dev/cu.usbmodemXXXX" # optional; leave unset to auto-detect
export ESP32DASH_SERIAL_BAUD="115200"           # optional
export ESP32DASH_STATE_DIR="/tmp/esp32dash"     # optional
export RUST_LOG="info"                          # optional
```

## Manual Run

```bash
cd tools/esp32dash
cargo run -- agent run
```

In another terminal:

```bash
printf '%s\n' '{"hook_event_name":"SessionStart","cwd":"/tmp/project","session_id":"sess-1"}' \
  | cargo run -- claude ingest --event-from-stdin
```

Then inspect state:

```bash
cargo run -- agent status
```

## Claude Code Hook

Install the hook wrapper and update `~/.claude/settings.json` automatically:

```bash
cargo run -- install-hooks
```

Use `-f` to skip the interactive confirmation:

```bash
cargo run -- install-hooks --force
```

The command writes `~/.claude/hooks/esp32dash-hook.sh`, preserves existing Claude hooks, and only appends any missing `esp32dash` entries in `~/.claude/settings.json`.

Running it again is safe. If the hook script and settings entries are already in place, the command becomes a no-op. `-f` only skips the confirmation prompt before writing changes.

The hook config added by `install-hooks` covers:

- `SessionStart`
- `SessionEnd`
- `Notification`
- `UserPromptSubmit`
- `PreToolUse`
- `PostToolUse`
- `PostToolUseFailure`
- `PermissionDenied`
- `Elicitation`
- `ElicitationResult`
- `Stop`
- `StopFailure`
- `SubagentStart`
- `SubagentStop`
- `PreCompact`
- `PostCompact`
- `PermissionRequest`

On the device, true `PermissionRequest` events keep the actionable `Accept / Decline / YOLO`
overlay. `AskUserQuestion` and `Elicitation` waits use a separate read-only prompt overlay
that shows the question and points the user back to the terminal.

## launchd

Install once from a stable executable path:

```bash
cd tools/esp32dash
cargo install --path .
ESP32DASH_SERIAL_PORT=/dev/cu.usbmodemXXXX ~/.cargo/bin/esp32dash install-launchd
```

The command writes `~/Library/LaunchAgents/com.local.esp32dash.plist` and starts the new agent.
Run it as your logged-in macOS user, not with `sudo`: this is a per-user `LaunchAgent`, and `sudo` will incorrectly target root's unsupported `gui/0` domain.

For quick local testing you can still use `cargo run -- install-launchd`, but the plist will point at the current build artifact under `target/`, so `cargo clean` or moving the repo will break the service.

## Serial Protocol

Protocol frames share the same serial line as normal ESP-IDF logs. Only lines starting with `@esp32dash ` are treated as protocol messages.

Device hello:

```json
@esp32dash {"type":"hello","protocol_version":1,"device_id":"esp32-dashboard-a1b2c3","product":"Waveshare ESP32-S3-Touch-LCD-3.49","capabilities":["device.info","device.reboot","config.export","config.set_many","wifi.scan","wifi.profiles.list","wifi.profile.add","wifi.profile.remove","claude.update","claude.approval.request","claude.approval.dismiss","claude.approval.resolved","claude.prompt.request","claude.prompt.dismiss","screen.capture.start"]}
```

Host request:

```json
@esp32dash {"type":"request","id":"rpc-1","method":"wifi.scan","params":{}}
```

Device response:

```json
@esp32dash {"type":"response","id":"rpc-1","ok":true,"result":{"aps":[{"ssid":"my-network","rssi":-49,"auth_mode":"wpa2_psk","auth_required":true}]}}
```

Host Wi-Fi profile list:

```json
@esp32dash {"type":"request","id":"rpc-2","method":"wifi.profiles.list","params":{}}
```

Host Wi-Fi profile add/update:

```json
@esp32dash {"type":"request","id":"rpc-3","method":"wifi.profile.add","params":{"ssid":"my-network","password":"secret","hidden":false}}
```

Host config commit:

```json
@esp32dash {"type":"request","id":"rpc-4","method":"config.set_many","params":{"items":[{"key":"time.timezone_name","value":"Asia/Shanghai"},{"key":"time.timezone_tz","value":"CST-8"},{"key":"weather.city_label","value":"Shanghai"},{"key":"weather.latitude","value":"31.2304"},{"key":"weather.longitude","value":"121.4737"}]}}
```

Host Claude update:

```json
@esp32dash {"type":"event","method":"claude.update","payload":{"seq":7,"source":"claude_code","status":"waiting_for_input","title":"Ready for input","workspace":"project-foo","detail":"Previous action completed","permission_mode":"default","ts":1743957128,"unread":true,"attention":"medium","session_id":"sess-1","event":"Stop"}}
```

Host approval request:

```json
@esp32dash {"type":"event","method":"claude.approval.request","payload":{"id":"approval-1","tool_name":"Bash","description":"rm -rf /tmp/test"}}
```

Host approval dismiss:

```json
@esp32dash {"type":"event","method":"claude.approval.dismiss","payload":{"id":"approval-1"}}
```

Host read-only prompt request:

```json
@esp32dash {"type":"event","method":"claude.prompt.request","payload":{"id":"prompt-1","kind":"question_prompt","title":"Plan ready","question":"Execute now or revise?","options_text":"1. Execute - Run the plan  2. More prompt - Edit in terminal"}}
```

Host read-only prompt dismiss:

```json
@esp32dash {"type":"event","method":"claude.prompt.dismiss","payload":{"id":"prompt-1"}}
```

Host screenshot capture start:

```json
@esp32dash {"type":"request","id":"rpc-5","method":"screen.capture.start","params":{}}
```

Device screenshot capture metadata:

```json
@esp32dash {"type":"response","id":"rpc-5","ok":true,"result":{"capture_id":"capture-1","app":"home","source":"lvgl","format":"rgb565_le","width":640,"height":172,"stride_bytes":1280,"data_size":220160,"chunk_bytes":1024,"chunk_count":215}}
```

Device screenshot chunk:

```json
@esp32dash {"type":"event","method":"screen.capture.chunk","payload":{"capture_id":"capture-1","index":0,"data":"...base64..."}}
```

Device screenshot completion:

```json
@esp32dash {"type":"event","method":"screen.capture.done","payload":{"capture_id":"capture-1","chunks_sent":215,"bytes_sent":220160}}
```
