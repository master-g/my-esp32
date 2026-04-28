## ADDED Requirements

### Requirement: Device link prompt resolution SHALL be atomic
The `device_link_resolve_prompt` function MUST prepare the complete event payload while holding the state mutex to prevent race conditions with concurrent prompt updates.

#### Scenario: Resolve prompt without race condition
- **WHEN** a prompt is resolved by the device
- **THEN** the function copies both the prompt ID and selection index while holding the mutex
- **AND** constructs the full JSON payload before releasing the mutex
- **AND** sends the event after releasing the mutex

#### Scenario: New prompt arrives during resolution
- **WHEN** a new prompt request arrives while the previous one is being resolved
- **THEN** the resolution event contains the correct ID and selection for the OLD prompt
- **AND** the new prompt is stored separately without corruption

### Requirement: Prompt and approval IDs SHALL use separate sequences
The host agent MUST maintain separate atomic counters for prompt IDs and approval IDs to ensure non-interleaved sequence numbers.

#### Scenario: Multiple prompts and approvals
- **WHEN** two prompts and one approval are created in sequence
- **THEN** prompt IDs are "prompt-1", "prompt-2"
- **AND** approval IDs are "approval-1"
- **AND** NOT interleaved sequences like "prompt-1", "approval-2", "prompt-3"
