## Context

esp32dash is a host agent for the ESP32-S3 dashboard that ingests Claude Code hook events and manages device interaction over USB serial. The current implementation only provides a blocking user-input path for `PermissionRequest` events. `Elicitation` and `AskUserQuestion` events are routed through a non-blocking `ingest` path, so the device displays a passive overlay but the user's selection is never captured.

The device protocol has `claude.approval.request/response/dismiss` for approvals, but only `claude.prompt.request/dismiss` for prompts — there is no `claude.prompt.response`.

## Goals / Non-Goals

**Goals:**
- All three user-input event types (PermissionRequest, Elicitation, AskUserQuestion) have a blocking path through the ESP32 device
- Clear semantic naming: `AllowAlways` replaces `Yolo`
- Configurable timeouts with sensible defaults
- `permission_suggestions` from Claude Code are forwarded to the device and used in "always allow" responses
- Elicitation form mode is handled gracefully (decline + redirect to terminal)

**Non-Goals:**
- Supporting complex form input on the ESP32 device (screen is 640×172, not viable)
- Supporting Elicitation URL mode (browser auth) on the device
- Multi-question AskUserQuestion (only first question is displayed, matching current behavior)
- Changing the ESP32 firmware approval overlay layout (keep 3-button design, only change icon/text)

## Decisions

### 1. Rename `claude approve` to `claude respond` (with `approve` as alias)

**Rationale**: The command now handles more than just approval — it handles Elicitation and AskUserQuestion responses too. `respond` is a more accurate name. The old `approve` name is kept as a hidden alias for backward compatibility with existing hook scripts.

### 2. Hook script routes by event name, not tool name

**Rationale**: The routing logic in `render_hook_script` checks `hook_event_name` for `PermissionRequest` and `Elicitation`, and checks `tool_name` for `AskUserQuestion` (which is a `PreToolUse` event). This keeps the shell script simple — one `grep` for event name, one for tool name.

### 3. Prompt response uses selection_index, not option text

**Rationale**: Sending the numeric index from the device is simpler and avoids string encoding issues over serial. The host maps the index back to the option label using the stored `WaitingPrompt.options` array.

### 4. `permission_suggestions` stored in ApprovalRequest, not forwarded raw to device

**Rationale**: The device doesn't need to understand the full `permission_suggestions` structure. It just needs to show the "Always" button. When the device responds with `allow_always`, the host uses the stored suggestions to construct the `updatedPermissions` output. This keeps the device firmware simple.

### 5. Elicitation form mode detected by checking for `requested_schema` properties

**Rationale**: If the `requested_schema` has `properties` with non-trivial fields (not just a single enum-like field), it's a form that can't be rendered on the ESP32. The host outputs `action: "decline"` immediately. If there are simple options, they're displayed on the device.

### 6. Timeout configuration in config.toml under `[approval]` section

**Rationale**: All approval/prompt timeouts are related to user interaction, so they belong in a single section. Device RPC timeout is derived automatically (store timeout + 10s) to avoid inconsistency.

### 7. Backward compatibility for `"yolo"` string from device

**Rationale**: Existing ESP32 firmware sends `"yolo"` in `claude.approval.resolved`. The host `parse_device_event` accepts both `"yolo"` and `"allow_always"`, mapping both to `ApprovalDecision::AllowAlways`. This allows phased firmware updates.

## Risks / Trade-offs

| Risk | Mitigation |
|------|-----------|
| Elicitation form mode decline may frustrate users who expect device interaction | Chibi sprite shows clear warning state; decline message explains to use terminal |
| selection_index could be out of bounds if device and host state diverge | Host validates index against stored options array length; out-of-bounds treated as dismiss |
| 10-minute approval timeout may feel long | Configurable; users can reduce in config.toml |
| AskUserQuestion multi-question only shows first question | Matches current behavior; documented limitation |
| Hook script grep for "AskUserQuestion" in tool_name is fragile | Uses exact string match in grep pattern; tool_name field is reliably present |
