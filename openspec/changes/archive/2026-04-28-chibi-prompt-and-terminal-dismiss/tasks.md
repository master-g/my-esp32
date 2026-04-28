## 1. Agent API - Add resolve endpoints

- [ ] 1.1 Add `POST /v1/claude/approvals/{id}/resolve` endpoint in `agent.rs`
- [ ] 1.2 Add `POST /v1/claude/prompts/{id}/resolve` endpoint in `agent.rs`
- [ ] 1.3 Implement `ApprovalStore::resolve_by_id` method in `approvals.rs`
- [ ] 1.4 Implement `PromptStore::resolve_by_id` method in `prompts.rs`
- [ ] 1.5 Wire new endpoints into the axum router

## 2. Chibi CLI - Add interactive prompt/approval commands

- [ ] 2.1 Add `ChibiCommand::Prompt` variant with options/question/title fields in `main.rs`
- [ ] 2.2 Add `ChibiCommand::Approval` variant with tool/input fields in `main.rs`
- [ ] 2.3 Implement `run_chibi_prompt` handler that sends `claude.prompt.request` via direct serial
- [ ] 2.4 Implement `run_chibi_approval` handler that sends `claude.approval.request` via direct serial
- [ ] 2.5 Add terminal UI for option selection (using inquire or similar crate)
- [ ] 2.6 Add terminal UI for approval decision (a/d/y keys)
- [ ] 2.7 Implement device response listener that prints the result

## 3. Terminal Hook Handlers - Fix dismiss after selection

- [ ] 3.1 In `handle_permission_request`, after terminal decision, call `POST /v1/claude/approvals/{id}/resolve`
- [ ] 3.2 In `handle_elicitation`, after terminal selection, call `POST /v1/claude/prompts/{id}/resolve`
- [ ] 3.3 In `handle_ask_user_question`, after terminal selection, call `POST /v1/claude/prompts/{id}/resolve`
- [ ] 3.4 Add error handling for resolve API failures (log warning but don't block)

## 4. Device UI - Improve overlay display

- [ ] 4.1 Increase approval button height from 32px to 36px in `home_approval.c`
- [ ] 4.2 Increase approval button font size for better readability
- [ ] 4.3 Adjust approval overlay padding and spacing
- [ ] 4.4 Increase prompt option button height to 36px in `home_prompt.c`
- [ ] 4.5 Adjust prompt question label position and spacing
- [ ] 4.6 Ensure option buttons have minimum width of 60px
- [ ] 4.7 Test UI changes on actual device

## 5. Testing & Verification

- [ ] 5.1 Test `chibi prompt` command with device connected
- [ ] 5.2 Test `chibi approval` command with device connected
- [ ] 5.3 Verify terminal selection dismisses device prompt overlay
- [ ] 5.4 Verify terminal selection dismisses device approval overlay
- [ ] 5.5 Verify device selection still works and prints to terminal
- [ ] 5.6 Run Rust tests: `cd tools/esp32dash && cargo test`
- [ ] 5.7 Build ESP32 firmware: `idf.py build`
