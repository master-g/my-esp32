## 1. Device Firmware - Remove prompt overlay, keep approval overlay

- [x] 1.1 In `home_runtime.c`, remove `APP_EVENT_PROMPT_REQUEST` case handler (no longer shows prompt overlay)
- [x] 1.2 In `home_runtime.c`, remove `APP_EVENT_PROMPT_DISMISS` case handler
- [x] 1.3 Keep `APP_EVENT_PERMISSION_REQUEST` and `APP_EVENT_PERMISSION_DISMISS` handlers unchanged
- [x] 1.4 In `device_link.c`, modify `handle_event_frame` for prompt events:
  - Store pending prompt state but do NOT call `focus_home_for_claude_overlay()`
  - Do NOT publish `APP_EVENT_PROMPT_REQUEST`
  - Keep `claude.prompt.dismiss` handling to clear internal state
- [x] 1.5 Keep `device_link.c` approval event handling unchanged
- [x] 1.6 Optionally disable or remove `home_prompt.c` overlay creation code

## 2. Device Firmware - Add sprite prompt reminder

- [x] 2.1 Add `has_pending_prompt` flag to device_link state or service_claude state
- [x] 2.2 Add function to check combined pending state:
  - If approval pending → "thinking" + "Need permission"
  - If prompt pending (and no approval) → "waiting" + "Need input"
  - If none → "idle" + clear bubble
- [x] 2.3 Integrate sprite state update into existing snapshot processing or periodic update
- [x] 2.4 Ensure sprite state updates are smooth and do not flicker
- [x] 2.5 Test sprite transitions: idle → waiting → idle, idle → thinking → idle

## 3. Host Agent - Modify prompt sync to use snapshot

- [x] 3.1 In `agent.rs`, modify `sync_device_approvals()`:
  - For approvals: keep existing `send_device_approval_request()` logic
  - For prompts: do NOT send `send_device_prompt_request()`
- [x] 3.2 Add `has_pending_prompt` field to Snapshot struct in `model.rs`
- [x] 3.3 Update snapshot builder to set `has_pending_prompt` based on `prompts.has_device_backlog()`
- [x] 3.4 Ensure snapshot is sent when prompt state changes (submit, resolve, dismiss)
- [x] 3.5 In `handle_elicitation` and `handle_ask_user_question`, after terminal selection, ensure snapshot is sent with updated state

## 4. Chibi CLI - Add interactive test commands

- [x] 4.1 Add `ChibiCommand::Approval` variant with `--tool`, `--input`, `--clear` flags
- [x] 4.2 Add `ChibiCommand::Prompt` variant with `--options`, `--question`, `--title`, `--clear` flags
- [x] 4.3 Implement `run_chibi_approval`:
  - Without `--clear`: sends `claude.approval.request` via serial, blocks for device response
  - With `--clear`: sends `claude.approval.dismiss`
- [x] 4.4 Implement `run_chibi_prompt`:
  - Without `--clear`: sends `claude.prompt.request` via serial, does NOT block
  - With `--clear`: sends `claude.prompt.dismiss`
- [x] 4.5 Update help text and command documentation

## 5. Testing & Verification

- [x] 5.1 Flash firmware and verify approval overlay still appears for PermissionRequest
- [x] 5.2 Verify prompt does NOT show overlay (only sprite "waiting" + "Need input")
- [x] 5.3 Verify terminal displays Claude Code's prompt dialog normally
- [x] 5.4 After terminal prompt selection, verify sprite returns to "idle"
- [x] 5.5 Test `chibi approval` triggers overlay and blocks for response
- [x] 5.6 Test `chibi prompt` triggers sprite reminder (non-blocking)
- [x] 5.7 Test simultaneous approval + prompt: overlay shows, sprite shows "thinking"
- [x] 5.8 Run Rust tests: `cd tools/esp32dash && cargo test`
- [x] 5.9 Build ESP32 firmware: `idf.py build`
- [x] 5.10 Verify existing approval tests still pass
