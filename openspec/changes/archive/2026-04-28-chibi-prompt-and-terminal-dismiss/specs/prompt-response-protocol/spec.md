## MODIFIED Requirements

### Requirement: Device can respond to prompts
The ESP32 device can send a `claude.prompt.response` event to the host with the user's selection from an Elicitation or AskUserQuestion prompt. Additionally, when the user resolves the prompt through the terminal (not the device), the host SHALL send a dismiss event to the device.

#### Scenario: Device responds with selection index
- **WHEN** the device displays a prompt with options and the user selects one
- **THEN** the device sends `claude.prompt.response` with `{ "id": "<transport_id>", "selection_index": <n> }`
- **AND** the host resolves the pending prompt and unblocks the polling `claude respond` process

#### Scenario: Host outputs Elicitation response
- **WHEN** the host receives a `claude.prompt.response` for an Elicitation prompt
- **THEN** `claude respond` outputs `{ "hookSpecificOutput": { "hookEventName": "Elicitation", "action": "accept", "content": { "selected": "<option_label>" } } }`

#### Scenario: Host outputs AskUserQuestion response
- **WHEN** the host receives a `claude.prompt.response` for an AskUserQuestion prompt
- **THEN** `claude respond` outputs the PreToolUse permissionDecision JSON with `updatedInput` containing the original questions array and an `answers` object mapping each question's text to the selected option's label

#### Scenario: Prompt dismissed before response
- **WHEN** a follow-up event dismisses the prompt before the device responds
- **THEN** the pending prompt resolution is dismissed and `claude respond` exits with no output

#### Scenario: Terminal selection dismisses device prompt
- **WHEN** the user resolves the prompt through the terminal interface (not the device)
- **THEN** the host resolves the pending prompt internally
- **AND** the host sends `claude.prompt.dismiss` to the device
- **AND** the device hides the prompt overlay

## ADDED Requirements

### Requirement: Agent SHALL expose resolve API for external prompt resolution
The agent SHALL provide HTTP endpoints that allow external clients to resolve a pending prompt.

#### Scenario: Resolve prompt via API
- **WHEN** a client sends `POST /v1/claude/prompts/{id}/resolve` with `{ "selection_index": <n> }`
- **THEN** the agent resolves the pending prompt with the given selection
- **AND** the agent triggers `sync_device_approvals()` to send dismiss to the device if visible

### Requirement: Agent SHALL expose resolve API for external approval resolution
The agent SHALL provide HTTP endpoints that allow external clients to resolve a pending approval.

#### Scenario: Resolve approval via API
- **WHEN** a client sends `POST /v1/claude/approvals/{id}/resolve` with `{ "decision": "allow" | "deny" | "allow_always" }`
- **THEN** the agent resolves the pending approval with the given decision
- **AND** the agent triggers `sync_device_approvals()` to send dismiss to the device if visible
