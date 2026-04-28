## ADDED Requirements

### Requirement: Unified respond command for user-input events

The `claude respond` CLI subcommand handles all hook events that require user interaction on the ESP32 device.

#### Scenario: PermissionRequest routed to respond command
- **WHEN** hook script receives a `PermissionRequest` event
- **THEN** the event is routed to `claude respond --event-from-stdin`, which submits an approval to the agent, polls for the device response, and outputs the PermissionRequest decision JSON to stdout

#### Scenario: Elicitation with options routed to respond command
- **WHEN** hook script receives an `Elicitation` event with options in the requested schema
- **THEN** the event is routed to `claude respond --event-from-stdin`, which submits a prompt to the agent, polls for the device selection, and outputs the Elicitation response JSON to stdout

#### Scenario: Elicitation with form mode (no options)
- **WHEN** hook script receives an `Elicitation` event with a `requested_schema` containing complex form fields (no simple options)
- **THEN** `claude respond` outputs `action: "decline"` immediately without submitting to the device, and the chibi sprite displays a warning state directing the user to the terminal

#### Scenario: AskUserQuestion routed to respond command
- **WHEN** hook script receives a `PreToolUse` event for tool `AskUserQuestion`
- **THEN** the event is routed to `claude respond --event-from-stdin`, which submits a prompt to the agent, polls for the device selection, and outputs the PreToolUse permissionDecision JSON with updatedInput containing the answers

#### Scenario: Respond command timeout
- **WHEN** the device does not respond within the configured timeout
- **THEN** `claude respond` outputs a deny/decline response and logs a warning

#### Scenario: Agent unavailable fallback
- **WHEN** the local esp32dash agent is not running
- **THEN** `claude respond` exits cleanly (exit 0 with no output), allowing Claude Code to show its normal permission/dialog UI
