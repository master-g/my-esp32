# esp32dash

`esp32dash` is the host-side companion CLI for the dashboard firmware. It runs a local agent for Claude Code hook ingestion, keeps the latest normalized Claude snapshot, and talks to the ESP32 over USB serial for configuration and management.

`esp32dash config` now drives an interactive setup flow:

- scan visible Wi-Fi SSIDs or enter a hidden SSID manually
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
cargo run -- config
cargo run -- install-launchd
cargo run -- uninstall-launchd
cargo run -- install-hooks
cargo run -- chibi approve-dismiss --delay-ms 1500
```

`chibi approve-dismiss` prefers the real host-side approval flow. When the local agent is reachable, it posts a synthetic `PermissionRequest`, submits a matching approval, then posts a follow-up `PreToolUse` event so the agent dismisses the ESP32 overlay through the same path used by normal Claude hooks. If the local agent is unavailable, it falls back to a direct serial smoke test and first forces `home.screensaver` off so the overlay is visible.

## Environment

```bash
export ESP32DASH_ADMIN_ADDR="127.0.0.1:37125"   # optional
export ESP32DASH_SERIAL_PORT="/dev/cu.usbmodem11401" # optional
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
- `Stop`
- `SubagentStop`
- `PreCompact`
- `PermissionRequest`

## launchd

Install once:

```bash
cd tools/esp32dash
ESP32DASH_SERIAL_PORT=/dev/cu.usbmodem11401 cargo run -- install-launchd
```

The command writes `~/Library/LaunchAgents/com.local.esp32dash.plist` and starts the new agent.

## Serial Protocol

Protocol frames share the same serial line as normal ESP-IDF logs. Only lines starting with `@esp32dash ` are treated as protocol messages.

Device hello:

```json
@esp32dash {"type":"hello","protocol_version":1,"device_id":"esp32-dashboard-a1b2c3","product":"Waveshare ESP32-S3-Touch-LCD-3.49","capabilities":["device.info","device.reboot","config.export","config.set_many","wifi.scan","claude.update","claude.approval.request","claude.approval.dismiss","claude.approval.resolved"]}
```

Host request:

```json
@esp32dash {"type":"request","id":"rpc-1","method":"wifi.scan","params":{}}
```

Device response:

```json
@esp32dash {"type":"response","id":"rpc-1","ok":true,"result":{"aps":[{"ssid":"my-network","rssi":-49,"auth_mode":"wpa2_psk","auth_required":true}]}}
```

Host config commit:

```json
@esp32dash {"type":"request","id":"rpc-2","method":"config.set_many","params":{"items":[{"key":"wifi.ssid","value":"my-network"},{"key":"wifi.password","value":"secret"},{"key":"time.timezone_name","value":"Asia/Shanghai"},{"key":"time.timezone_tz","value":"CST-8"},{"key":"weather.city_label","value":"Shanghai"},{"key":"weather.latitude","value":"31.2304"},{"key":"weather.longitude","value":"121.4737"}]}}
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
