---
title: "fix: Route Elicitation and AskUserQuestion hooks non-blocking"
type: fix
status: completed
date: 2026-05-12
---

# fix: Route Elicitation and AskUserQuestion hooks non-blocking

## Summary

Change the esp32dash hook script routing so only `PermissionRequest` goes through the blocking `respond` path. `Elicitation` and `PreToolUse(AskUserQuestion)` are routed to the non-blocking `ingest` path, allowing Claude Code to show its native CLI dialog while events are still forwarded to the ESP32 dashboard.

---

## Problem Frame

The current hook script routes three interactive event types — `PermissionRequest`, `Elicitation`, and `PreToolUse(AskUserQuestion)` — through a blocking `claude respond` path that polls the ESP32 device for an answer and outputs `hookSpecificOutput` to stdout. This replaces Claude Code's native CLI dialog, preventing users from answering on the terminal when they prefer terminal interaction (see origin discussion). The agent-unavailable case is already handled — `handle_elicitation` and `handle_ask_user_question` return `Ok(())` with no stdout on agent error, which triggers native fallback via the same exit-0-no-JSON rule.

Per the official Claude Code hooks documentation, when a command hook exits 0 with no JSON output, Claude Code falls back to its native UI. The `ingest` path already does this — it fires a POST to the agent for forwarding to ESP32 and exits 0. Routing `Elicitation` and `AskUserQuestion` to `ingest` preserves ESP32 dashboard visibility without consuming the CLI dialog.

`PermissionRequest` is kept on the `respond` path because tool execution permissions are a safety gate where device-based approval adds meaningful value even at the cost of blocking CLI.

---

## Requirements

- R1. `PermissionRequest` events continue to use the blocking `respond` path (no behavior change)
- R2. `Elicitation` events route to the non-blocking `ingest` path, allowing Claude Code's native elicitation dialog to appear
- R3. `PreToolUse` events with `tool_name` of `AskUserQuestion` route to the non-blocking `ingest` path, allowing Claude Code's native question dialog to appear
- R4. All other hook events (SessionStart, PostToolUse, Notification, etc.) remain on `ingest` path (no behavior change)
- R5. Existing tests in `hooks.rs` continue to pass

---

## Scope Boundaries

- Only the hook script routing logic in `render_hook_script` is changed
- The `respond` handler code (`handle_permission_request`, `handle_elicitation`, `handle_ask_user_question` in `main.rs`) is preserved as-is for potential future re-enablement and direct CLI usage
- No changes to agent, firmware, config, or installation logic

### Deferred to Follow-Up Work

- Removing the now-dead `respond` handler code for Elicitation and AskUserQuestion: deferred until the change proves stable in practice
- Config-based routing toggle per event type: future enhancement

---

## Context & Research

### Relevant Code and Patterns

- `tools/esp32dash/src/hooks.rs:233-272` — `render_hook_script` generates the shell script that routes events to `respond` or `ingest`
- `tools/esp32dash/src/hooks.rs:427-546` — test module for hook installation logic
- `tools/esp32dash/src/main.rs:1371-1802` — `handle_permission_request`, `handle_elicitation`, `handle_ask_user_question` (respond handlers, unchanged)
- `tools/esp32dash/src/main.rs:1020-1046` — `ingest_from_stdin` (non-blocking path)

### External References

- Claude Code hooks reference: hook exiting 0 with no JSON output causes native UI fallback
- `async: true` on command hooks runs in background but cannot make decisions (irrelevant for this fix)
- Claude Code hooks guide: PermissionRequest and Elicitation decision control via `hookSpecificOutput`

---

## Key Technical Decisions

- **Only PermissionRequest stays on respond path**: Device-side approval is the product's differentiator — permission gates are the interaction ESP32dash is built around. The CLI-preference argument that justifies moving Elicitation and AskUserQuestion to `ingest` does not apply symmetrically here: questions and elicitations are incidental to the user's workflow, while tool-permission approval is the canonical thing the device is for. Users who want CLI-side permission approval today can disable the hook or edit the shell script directly; a timeout-based fallback is deferred until that friction is reported.
- **Remove routing conditions rather than adding config**: A boolean toggle per event adds complexity without a demonstrated need. Users who want different routing can edit the shell script directly.
- **Preserve respond handler code**: Removing it now would be premature. The handlers still serve direct CLI usage (`esp32dash claude respond --event-from-stdin`) and may be re-enabled later.

---

## Implementation Units

- U1. **Update hook script routing in `render_hook_script`**

**Goal:** Change the shell script generation so only `PermissionRequest` routes to the `respond` path; `Elicitation` and `PreToolUse(AskUserQuestion)` route to `ingest`.

**Requirements:** R1, R2, R3, R4

**Dependencies:** None

**Files:**
- Modify: `tools/esp32dash/src/hooks.rs`

**Approach:**
- In `render_hook_script`, the `IS_RESPOND_EVENT` logic currently checks three conditions. Replace the `Elicitation` and `AskUserQuestion` conditions so only `PermissionRequest` sets `IS_RESPOND_EVENT=1`. The `PreToolUse` + `AskUserQuestion` `elif` branch is removed entirely.
- The `else` branch (ingest + exit 0) now covers Elicitation and AskUserQuestion, which is the desired behavior.

**Patterns to follow:**
- Existing shell script generation pattern in `render_hook_script` (heredoc-style format string)

**Test scenarios:**
- Happy path: `render_hook_script` output contains `IS_RESPOND_EVENT=1` only for `PermissionRequest` condition, not for `Elicitation` or `AskUserQuestion`
- Happy path: The generated script routes `PermissionRequest` through `claude respond` and `Elicitation` through `claude ingest`
- Edge case: The `PreToolUse` + `AskUserQuestion` check is absent from the generated script

**Pin routing with a new test:** Add a unit test `render_hook_script_routes_only_permission_request_to_respond` in `hooks.rs` that calls `render_hook_script(Path::new("/tmp/esp32dash"))`, then asserts the rendered string contains the `[ "$EVENT_NAME" = "PermissionRequest" ]` condition and does NOT contain `Elicitation` or `AskUserQuestion` tokens inside the `IS_RESPOND_EVENT` block. Without this, `analyze_install_detects_existing_state` is self-consistent by construction (it regenerates via `render_hook_script`) and cannot catch a regression that re-adds Elicitation to the respond branch.

**Verification:**
- `cargo test` passes in `tools/esp32dash/`
- `cargo test -- analyze_install_detects_existing_state` — this test calls `render_hook_script` and confirms the script is stable
- `cargo test -- render_hook_script_routes_only_permission_request_to_respond` — the new routing-pin test passes

---

- U2. **Verify existing test still passes with regenerated script**

**Goal:** Verify the test that compares generated hook script content still passes with the new routing logic.

**Requirements:** R5

**Dependencies:** U1

**Files:**
- Modify: `tools/esp32dash/src/hooks.rs`

**Approach:**
- The test `analyze_install_detects_existing_state` (line 526) calls `render_hook_script` and writes the result to disk, then verifies `analyze_install` detects no changes. After U1 changes the script content, this test's expected script must match the new output. Since the test generates the expected script by calling `render_hook_script` itself and comparing against a re-read, it should pass automatically after the U1 change — no assertion text needs updating. Verify this is the case.

**Patterns to follow:**
- Existing test structure in `hooks.rs` test module

**Test scenarios:**
- Happy path: `analyze_install_detects_existing_state` passes with the new script content

**Verification:**
- `cargo test` in `tools/esp32dash/` — all tests pass

---

## Risks & Dependencies

| Risk | Mitigation |
|------|------------|
| User relies on ESP32 to answer Elicitation prompts | The ESP32 still receives the event via ingest for dashboard display; user answers on CLI instead. Acceptable tradeoff per scope decision. |
| AskUserQuestion hook returning empty on PreToolUse causes Claude Code to show its own dialog (correct behavior) | Verified against official docs: exit 0 with no JSON = native fallback |

---

## Sources & References

- Origin discussion: conversation analysis of esp32dash hook blocking behavior
- `tools/esp32dash/src/hooks.rs:233-272` — current `render_hook_script` implementation
- Claude Code hooks reference: PermissionRequest, Elicitation, PreToolUse decision control documentation
