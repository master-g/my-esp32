## Context

The `hooks-user-input-refactor` change introduced three new user-input paths (`PermissionRequest`, `Elicitation`, `AskUserQuestion`) with a unified `claude respond` command. A code review of that change revealed several bugs ranging from memory leaks to UI usability issues and race conditions. This change fixes those bugs to make the prompt/approval system production-ready.

**Current State:**
- `PromptStore` stores pending prompts but never removes them, causing unbounded memory growth
- Device prompt buttons show only "1", "2", "3", "4" without option text
- `device_link_resolve_prompt` copies the prompt ID, releases the mutex, then sends the event — a new prompt could overwrite state between release and send
- Prompt and approval IDs share a single atomic counter, making logs hard to read
- Elicitation with empty options shows a blank button row instead of a helpful hint
- Form mode detection only checks for `properties`, missing schemas that use `allOf` or `anyOf`

## Goals / Non-Goals

**Goals:**
- Prevent memory leaks from unresolved prompts via timeout-based cleanup
- Make device prompt buttons usable by showing option text
- Fix race condition in prompt resolution to prevent incorrect event payloads
- Improve observability with separate ID sequences for prompts and approvals
- Handle edge cases in elicitation parsing and form mode detection gracefully

**Non-Goals:**
- No changes to the wire protocol between device and host
- No changes to the public CLI interface (`claude respond`)
- No new features (only bug fixes and polish)

## Decisions

1. **PromptStore cleanup: Timeout-based with background task**
   - Rationale: `ApprovalStore` already uses this pattern (configurable timeout + background cleanup task). Consistency across stores reduces cognitive load.
   - Alternative considered: Manual cleanup in every code path that could dismiss a prompt — rejected because it's error-prone and misses edge cases.

2. **Option text on buttons: Truncate to fit button width**
   - Rationale: The ESP32 screen is 640×172. Button width is limited (~120px). Showing full option text would overflow. Truncating to ~10 characters with ellipsis provides enough context.
   - Alternative considered: Multi-line buttons or scrolling text — rejected because LVGL button labels don't support wrapping well at small sizes.

3. **Race condition fix: Prepare payload while holding mutex**
   - Rationale: The simplest fix is to allocate the payload string while the mutex is held, then send after release. This avoids copying complex state out of the critical section.
   - Alternative considered: Use a second mutex for event sending — rejected because it adds complexity without benefit.

4. **Separate atomic counters: `NEXT_PROMPT_ID_SEQ` and `NEXT_APPROVAL_ID_SEQ`**
   - Rationale: Zero cost, makes logs immediately readable (e.g., "prompt-123" vs "approval-456" instead of interleaved "prompt-1", "approval-2", "prompt-3").

5. **Form mode detection: Decline any schema with `type: object`**
   - Rationale: More conservative than checking only `properties`. Any schema that describes an object structure is too complex for a 3-button device UI. This catches `allOf`, `anyOf`, `oneOf` compositions that resolve to objects.
   - Alternative considered: Recursively inspect schema for any object type — rejected as overkill; `type: object` at root is sufficient.

## Risks / Trade-offs

- **[Risk]** Truncated option text may still be ambiguous for similar options (e.g., "Buy A..." vs "Buy B...")
  - **Mitigation**: Users can still see the full prompt text above the buttons. If ambiguity is common, we can add a secondary label or tooltip in a future change.
- **[Risk]** Timeout-based cleanup could discard a prompt that the user is actively considering
  - **Mitigation**: Use the same timeout as approvals (default 600s), which is long enough for deliberate decisions. Timeouts are configurable.
