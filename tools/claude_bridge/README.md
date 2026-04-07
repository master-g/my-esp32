# Claude Bridge

Rust daemon and CLI for forwarding Claude Code hook events to local devices over WebSocket.

## Commands

```bash
cargo run -- daemon
cargo run -- send --event-from-stdin
cargo run -- status
cargo run -- install-launchd
cargo run -- uninstall-launchd
```

## Environment

```bash
export CLAUDE_BRIDGE_PSK="your-pre-shared-token"
export CLAUDE_BRIDGE_ADMIN_ADDR="127.0.0.1:37125"   # optional
export CLAUDE_BRIDGE_WS_ADDR="0.0.0.0:8765"         # optional
export CLAUDE_BRIDGE_STATE_DIR="/tmp/claude-bridge" # optional
export RUST_LOG="info"                              # optional
```

`CLAUDE_BRIDGE_PSK` is required for `daemon` and `install-launchd`.

## Manual Run

```bash
cd tools/claude_bridge
cargo run -- daemon
```

In another terminal:

```bash
printf '%s\n' '{"hook_event_name":"SessionStart","cwd":"/tmp/project","session_id":"sess-1"}' \
  | cargo run -- send --event-from-stdin
```

Then inspect state:

```bash
cargo run -- status
```

## Claude Code Hook

Use [hooks/claude-bridge-hook.sh](./hooks/claude-bridge-hook.sh) as the hook command target. It forwards hook stdin to `claude-bridge send --event-from-stdin`.

Example hook command:

```json
{
  "hooks": {
    "SessionStart": [
      {
        "hooks": [
          {
            "type": "command",
            "command": "/absolute/path/to/tools/claude_bridge/hooks/claude-bridge-hook.sh"
          }
        ]
      }
    ]
  }
}
```

Register the same shell wrapper for:

- `Notification`
- `UserPromptSubmit`
- `PreToolUse`
- `PostToolUse`
- `Stop`
- `SubagentStop`
- `PreCompact`
- `SessionStart`
- `SessionEnd`

## launchd

Install once:

```bash
cd tools/claude_bridge
CLAUDE_BRIDGE_PSK=your-token cargo run -- install-launchd
```

The command writes `~/Library/LaunchAgents/com.local.claude-bridge.plist`, bootstraps it with `launchctl`, and restarts the service.

## WebSocket Protocol

Device hello:

```json
{
  "type": "hello",
  "token": "psk-from-nvs",
  "device_id": "esp32-dashboard",
  "last_seq": 0
}
```

Daemon response:

- first message: `snapshot`
- optional follow-up messages: `delta`

Both have payload shape:

```json
{
  "type": "snapshot",
  "payload": {
    "seq": 1,
    "source": "claude_code",
    "session_id": "sess-1",
    "event": "PermissionRequest",
    "status": "waiting_for_input",
    "title": "Awaiting approval",
    "workspace": "project-foo",
    "detail": "exec_command requires approval (default)",
    "permission_mode": "default",
    "ts": 1743957123,
    "unread": true,
    "attention": "high"
  }
}
```
