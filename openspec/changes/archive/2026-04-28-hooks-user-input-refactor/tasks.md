## 1. ApprovalDecision refactoring

- [x] 1.1 Rename `ApprovalDecision::Yolo` to `ApprovalDecision::AllowAlways` in `approvals.rs`
- [x] 1.2 Update `parse_device_event` in `device.rs` to accept both `"yolo"` and `"allow_always"` strings, mapping to `AllowAlways`
- [x] 1.3 Update `permission_request_output_for_decision` in `main.rs` to handle `"allow_always"` with `permission_suggestions` support
- [x] 1.4 Update all tests referencing `Yolo` to use `AllowAlways`

## 2. Timeout configuration

- [x] 2.1 Add `ApprovalConfig` struct to `config.rs` with fields: `timeout_secs` (default 600), `elicitation_timeout_secs` (default 120), `question_timeout_secs` (default 180)
- [x] 2.2 Add `[approval]` section parsing to `FileAppConfig` and `merge_file_config`
- [x] 2.3 Update `default_config_template` to include `[approval]` section with commented defaults
- [x] 2.4 Replace hardcoded `APPROVAL_TIMEOUT` in `approvals.rs` with config value
- [x] 2.5 Replace hardcoded `300s` deadline in `approve_from_stdin` with config value
- [x] 2.6 Derive `RPC_TIMEOUT_APPROVAL` from config value (timeout + 10s) in `device.rs`
- [x] 2.7 Fix `RPC_TIMEOUT_APPROVAL(310s) > APPROVAL_TIMEOUT(300s)` inconsistency

## 3. Hook routing expansion

- [x] 3.1 Update `render_hook_script` in `hooks.rs` to route `Elicitation` and `PreToolUse/AskUserQuestion` to the blocking respond path
- [x] 3.2 Add `grep` check for `AskUserQuestion` tool name in the hook script routing logic
- [x] 3.3 Verify `HOOK_SPECS` already registers `Elicitation[*]` and `PreToolUse[*]` (no change needed)

## 4. Unified respond command

- [x] 4.1 Rename `ClaudeCommand::Approve` to `ClaudeCommand::Respond` in `main.rs`, keeping `approve` as a hidden alias
- [x] 4.2 Create `respond_from_stdin` function that detects `hook_event_name` and dispatches to the appropriate handler
- [x] 4.3 Implement `handle_permission_request` (extract from current `approve_from_stdin`)
- [x] 4.4 Implement `handle_elicitation` — parse Elicitation input, detect form mode vs options mode, submit prompt or decline immediately
- [x] 4.5 Implement `handle_ask_user_question` — parse PreToolUse/AskUserQuestion input, submit prompt, output permissionDecision with updatedInput/answers on response
- [x] 4.6 Implement `ElicitationFormChecker` — detect if `requested_schema` has complex fields that can't be rendered on ESP32
- [x] 4.7 Implement Elicitation output formatters: `elicitation_output_accept`, `elicitation_output_decline`
- [x] 4.8 Implement AskUserQuestion output formatter: `ask_user_question_output` with answers map

## 5. Prompt response protocol (esp32dash host)

- [x] 5.1 Add `DeviceEvent::PromptResolved { id: String, selection_index: u8 }` variant to `device.rs`
- [x] 5.2 Add `"claude.prompt.response"` parsing in `parse_device_event`
- [x] 5.3 Add `PromptStore::resolve` method that resolves a pending prompt by transport_id and selection_index
- [x] 5.4 Add `PromptStore::wait_for_selection` method (similar to `ApprovalStore::wait_for_decision`)
- [x] 5.5 Handle `DeviceEvent::PromptResolved` in the agent's device event loop (`build_app_state`)
- [x] 5.6 Add prompt response submission endpoint or wire through existing approval flow

## 6. permission_suggestions forwarding

- [x] 6.1 Add `permission_suggestions: Vec<Value>` field to `ApprovalRequest` in `approvals.rs`
- [x] 6.2 Parse `permission_suggestions` from hook input JSON in `approve_from_stdin` / `handle_permission_request`
- [x] 6.3 Include `permission_suggestions` in the approval submission HTTP body
- [x] 6.4 Update `send_device_approval_request` in `agent.rs` to include `suggestions` in the device event payload
- [x] 6.5 Update `DeviceApproval` struct to carry `permission_suggestions` for use in output formatting
- [x] 6.6 Update `permission_request_output_for_decision` for `"allow_always"` to use stored suggestions as `updatedPermissions` when available, falling back to a simple `addRules` for the tool

## 7. ESP32 firmware: Approval UI update

- [x] 7.1 Rename `APPROVAL_DECISION_YOLO` to `APPROVAL_DECISION_ALLOW_ALWAYS` in `device_link.h`
- [x] 7.2 Update `approval_decision_t` enum values
- [x] 7.3 Update `device_link_resolve_approval` to send `"allow_always"` string instead of `"yolo"`
- [x] 7.4 Update `home_approval.c` button: change icon from `LV_SYMBOL_WARNING` to `LV_SYMBOL_REFRESH`, change text from `"YOLO"` to `"Always"`
- [x] 7.5 Update `home_approval.h` struct field name from `btn_yolo` to `btn_always`

## 8. ESP32 firmware: Prompt response protocol

- [x] 8.1 Add `device_link_resolve_prompt(uint8_t selection_index)` function to `device_link.c`
- [x] 8.2 Implement `claude.prompt.response` event sending with `{ "id": "...", "selection_index": N }` payload
- [x] 8.3 Update `home_prompt.c` to display tappable option buttons when `option_count > 0`
- [x] 8.4 Add touch handler for prompt options that calls `device_link_resolve_prompt(index)`
- [x] 8.5 When no options available (form mode), display warning state with "Answer in terminal" hint (existing behavior)
- [x] 8.6 Add `home_prompt_resolve` or similar callback for when user selects an option

## 9. Integration testing

- [ ] 9.1 Test PermissionRequest flow end-to-end: hook → respond → device → allow → output
- [ ] 9.2 Test PermissionRequest with allow_always: verify `updatedPermissions` uses `permission_suggestions`
- [ ] 9.3 Test Elicitation with options: hook → respond → device → selection → output
- [ ] 9.4 Test Elicitation form mode: hook → respond → immediate decline → output
- [ ] 9.5 Test AskUserQuestion: hook → respond → device → selection → output with answers
- [ ] 9.6 Test timeout behavior: verify configurable timeouts work correctly
- [ ] 9.7 Test backward compatibility: old firmware sending `"yolo"` still works
- [ ] 9.8 Test agent unavailable fallback: respond exits cleanly with no output
- [ ] 9.9 Test prompt dismiss on follow-up event: verify cleanup
