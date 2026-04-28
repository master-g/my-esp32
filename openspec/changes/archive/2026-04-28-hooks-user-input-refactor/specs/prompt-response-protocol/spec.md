## ADDED Requirements

### Requirement: Device can respond to prompts

The ESP32 device can send a `claude.prompt.response` event to the host with the user's selection from an Elicitation or AskUserQuestion prompt.

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
