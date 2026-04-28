## Why

The previous hooks-user-input-refactor change introduced a `PromptStore` and device-side prompt UI, but a code review revealed critical and medium bugs that need fixing before the feature is production-ready. The most severe issue is a memory leak in `PromptStore` where unresolved prompts are never cleaned up. Additionally, the device prompt UI shows numbered buttons without option text, making it impossible for users to know what they're selecting. There are also race conditions and edge cases in the prompt resolution path that could lead to incorrect behavior or confusing UX.

## What Changes

- Add timeout-based cleanup to `PromptStore` to prevent memory leaks from unresolved prompts
- Update device prompt UI to display option text on buttons instead of just numbers
- Fix race condition in `device_link_resolve_prompt` by preparing payload while holding mutex
- Separate atomic counters for prompt and approval IDs to avoid interleaved sequences
- Improve `parse_elicitation_prompt_from_raw` fallback behavior when options are empty
- Make `handle_elicitation` form mode detection more conservative (decline any schema with `type: object`)

## Capabilities

### New Capabilities
- `prompt-store-cleanup`: Timeout-based expiration and cleanup for unresolved prompts in the host agent

### Modified Capabilities
- `device-prompt-ui`: Device prompt buttons must display option text, not just sequence numbers
- `device-link-atomicity`: Device link prompt resolution must be atomic to prevent race conditions

## Impact

- `tools/esp32dash/src/prompts.rs`: Add timeout mechanism to `PromptStore`
- `tools/esp32dash/src/main.rs`: Separate atomic counters for prompt and approval IDs
- `tools/esp32dash/src/device.rs`: Improve elicitation parsing and form mode detection
- `src/apps/app_home/src/home_prompt.c`: Display option text on prompt buttons
- `src/components/device_link/src/device_link.c`: Fix race condition in prompt resolution
- No breaking changes to public APIs or wire protocol
