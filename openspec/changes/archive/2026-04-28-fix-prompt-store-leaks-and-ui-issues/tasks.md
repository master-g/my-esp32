## 1. PromptStore Cleanup

- [x] 1.1 Add `timeout` and `created_at` fields to `PendingPrompt` struct
- [x] 1.2 Add configurable timeout to `PromptStore::new()` (reuse `ApprovalConfig`)
- [x] 1.3 Implement `cleanup_expired()` method that removes timed-out prompts
- [x] 1.4 Add background cleanup task (similar to `ApprovalStore`)
- [x] 1.5 Update `wait_for_selection` to return `Err(PromptTimeout)` on expiration
- [x] 1.6 Add unit tests for timeout behavior

## 2. Device Prompt UI

- [x] 2.1 Update `home_prompt.c` to receive option text alongside index
- [x] 2.2 Modify `create_option_btn` to display truncated option text (max 10 chars + ellipsis)
- [x] 2.3 Handle case where `option_count == 0` — show hint instead of blank buttons
- [x] 2.4 Update `home_prompt.h` if struct fields change
- [ ] 2.5 Flash firmware and verify button labels on device

## 3. Device Link Atomicity

- [x] 3.1 Fix `device_link_resolve_prompt` to construct payload while holding mutex
- [x] 3.2 Ensure prompt ID and selection index are copied before mutex release
- [x] 3.3 Add `NEXT_PROMPT_ID_SEQ` atomic counter in `main.rs`
- [x] 3.4 Update all prompt ID generation to use new counter
- [x] 3.5 Verify approval IDs still use `NEXT_APPROVAL_ID_SEQ`
- [x] 3.6 Add unit test for interleaved prompt/approval ID sequences

## 4. Elicitation Edge Cases

- [x] 4.1 Update `parse_elicitation_prompt_from_raw` to return `None` when options is empty
- [x] 4.2 Update caller to show hint when options are empty
- [x] 4.3 Make `handle_elicitation` form mode detection check for `type: object`
- [x] 4.4 Add unit tests for empty options and object schema detection
- [x] 4.5 Run full Rust test suite

## 5. Integration & Verification

- [x] 5.1 Build ESP32 firmware (`make build`)
- [x] 5.2 Flash firmware and test prompt display with options
- [x] 5.3 Test prompt timeout on device
- [x] 5.4 Verify no memory leaks in long-running agent
- [x] 5.5 Archive change after all tasks complete
