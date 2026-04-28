## Why

esp32dash currently only handles `PermissionRequest` events with a blocking path that allows user interaction on the ESP32 device. Two other event types that require user input — `Elicitation` (MCP server requests) and `AskUserQuestion` (Claude asks the user) — are routed through a non-blocking `ingest` path, meaning the device displays a passive overlay but the user's selection is never captured. This is a functional bug.

Additionally, the `ApprovalDecision::Yolo` variant has unclear semantics ("You Only Live Once" doesn't communicate "always allow"), timeouts are hardcoded and inconsistent, and `permission_suggestions` from Claude Code are ignored.

## What Changes

1. **Rename `Yolo` → `AllowAlways`** with clearer device UI (icon + label change)
2. **Expand hook routing** to block on `Elicitation` and `AskUserQuestion` in addition to `PermissionRequest`
3. **Add `claude respond` CLI subcommand** that unifies all three blocking event types
4. **Add prompt response protocol** (`claude.prompt.response`) so the device can send user selections back to the host
5. **Forward `permission_suggestions`** from Claude Code to the device, and use them when responding with "always allow"
6. **Make timeouts configurable** with sensible defaults (600s for approvals, 120s for elicitation, 180s for questions)
7. **Handle Elicitation form mode** gracefully — chibi sprite shows warning state, user returns to terminal

## Capabilities

### New Capabilities
- `unified-respond-command`: CLI subcommand that handles PermissionRequest, Elicitation, and AskUserQuestion with blocking device interaction
- `prompt-response-protocol`: Device-to-host event for returning user selections from Elicitation/AskUserQuestion prompts

### Modified Capabilities
- `approval-decision-model`: Rename Yolo to AllowAlways, add destination-aware permission persistence
- `hook-event-routing`: Expand blocking path to cover all user-input events
- `timeout-management`: Configurable timeouts replacing hardcoded values

## Impact

- **esp32dash Rust**: `approvals.rs`, `device.rs`, `agent.rs`, `main.rs`, `hooks.rs`, `config.rs`, `prompts.rs`, `model.rs`
- **ESP32 firmware**: `device_link.c/h`, `home_approval.c/h`, `home_prompt.c/h`
- **Device protocol**: New `claude.prompt.response` event, updated `claude.approval.request` with `suggestions` field, `allow_always` replaces `yolo` string
- **Hook script**: Updated routing logic in `render_hook_script`
- **Backward compatibility**: Host accepts both `"yolo"` and `"allow_always"` from device
