## 1. Device Firmware - Remove overlay display logic

- [ ] 1.1 In `home_runtime.c`, remove `APP_EVENT_PERMISSION_REQUEST` case handler (no longer shows approval overlay)
- [ ] 1.2 In `home_runtime.c`, remove `APP_EVENT_PROMPT_REQUEST` case handler (no longer shows prompt overlay)
- [ ] 1.3 In `home_runtime.c`, remove `APP_EVENT_PERMISSION_DISMISS` case handler
- [ ] 1.4 In `home_runtime.c`, remove `APP_EVENT_PROMPT_DISMISS` case handler
- [ ] 1.5 In `device_link.c`, modify `handle_event_frame` to store pending approval/prompt state but NOT call `focus_home_for_claude_overlay()` or `publish_ui_event()`
- [ ] 1.6 In `device_link.c`, keep `device_link_get_pending_approval()` and `device_link_get_pending_prompt()` for state queries
- [ ] 1.7 In `device_link.c`, keep `device_link_resolve_approval()` and `device_link_resolve_prompt()` but remove UI event publishing
- [ ] 1.8 Optionally remove or disable `home_approval.c` and `home_prompt.c` files (or keep for future use)

## 2. Device Firmware - Add sprite reminder state

- [ ] 2.1 In `service_claude` (or app_manager), add function to check pending approval/prompt state from device_link
- [ ] 2.2 Add logic to determine sprite emotion based on pending state:
  - pending approval → "thinking" emotion
  - pending prompt → "waiting" emotion
  - none → "idle" emotion
- [ ] 2.3 Add bubble text based on pending state:
  - pending approval → "Need permission"
  - pending prompt → "Need input"
  - none → clear bubble
- [ ] 2.4 Integrate sprite state update into the main LVGL task loop or event handling
- [ ] 2.5 Ensure sprite state updates are atomic and do not flicker

## 3. Host Agent - Simplify device sync

- [ ] 3.1 In `agent.rs`, modify `sync_device_approvals()` to send updated snapshot with pending status instead of forwarding approval/prompt requests
- [ ] 3.2 Remove `send_device_approval_request()` and `send_device_prompt_request()` from device sync path
- [ ] 3.3 Keep `send_device_dismiss()` and `send_device_prompt_dismiss()` for clearing reminders
- [ ] 3.4 Ensure snapshot includes pending approval/prompt status in a format the device can parse
- [ ] 3.5 In `handle_permission_request`, after terminal decision, send updated snapshot to device
- [ ] 3.6 In `handle_elicitation`, after terminal selection, send updated snapshot to device
- [ ] 3.7 In `handle_ask_user_question`, after terminal selection, send updated snapshot to device

## 4. Chibi CLI - Add reminder test commands

- [ ] 4.1 Add `ChibiCommand::Approval` variant with `--tool`, `--input`, and `--clear` flags in `main.rs`
- [ ] 4.2 Add `ChibiCommand::Prompt` variant with `--options`, `--question`, `--title`, and `--clear` flags in `main.rs`
- [ ] 4.3 Implement `run_chibi_approval` handler:
  - Without `--clear`: sends `claude.approval.request` via direct serial, prints confirmation
  - With `--clear`: sends `claude.approval.dismiss` via direct serial
- [ ] 4.4 Implement `run_chibi_prompt` handler:
  - Without `--clear`: sends `claude.prompt.request` via direct serial, prints confirmation
  - With `--clear`: sends `claude.prompt.dismiss` via direct serial
- [ ] 4.5 Add command examples to help text

## 5. Testing & Verification

- [ ] 5.1 Flash firmware and verify no approval overlay appears when PermissionRequest triggers
- [ ] 5.2 Verify sprite changes to "thinking" with "Need permission" bubble
- [ ] 5.3 Verify terminal displays Claude Code's permission dialog normally (not blocked)
- [ ] 5.4 After terminal selection, verify sprite returns to "idle"
- [ ] 5.5 Test `chibi approval` command triggers sprite reminder
- [ ] 5.6 Test `chibi approval --clear` removes sprite reminder
- [ ] 5.7 Test `chibi prompt` command triggers sprite reminder
- [ ] 5.8 Test `chibi prompt --clear` removes sprite reminder
- [ ] 5.9 Run Rust tests: `cd tools/esp32dash && cargo test`
- [ ] 5.10 Build ESP32 firmware: `idf.py build`
