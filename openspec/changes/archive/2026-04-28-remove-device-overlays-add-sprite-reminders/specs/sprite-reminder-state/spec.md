## ADDED Requirements

### Requirement: Sprite SHALL display reminder state for pending approvals
When the host agent has a pending approval (PermissionRequest), the chibi sprite SHALL change its emotion to indicate that user input is needed.

#### Scenario: Approval pending triggers thinking emotion
- **WHEN** the host receives a PermissionRequest and forwards it to the device
- **THEN** the device updates the chibi sprite emotion to "thinking"
- **AND** the sprite displays a bubble with text "Need permission"

#### Scenario: Terminal approval clears sprite reminder
- **WHEN** the user resolves the approval through the terminal (not the device)
- **THEN** the host sends an updated snapshot to the device
- **AND** the device updates the sprite emotion back to "idle"
- **AND** the bubble disappears

#### Scenario: Multiple approvals pending
- **WHEN** multiple approvals are pending simultaneously
- **THEN** the sprite maintains the "thinking" emotion
- **AND** the bubble text remains "Need permission"
- **AND** the sprite state does not flicker between different emotions

### Requirement: Sprite SHALL display reminder state for pending prompts
When the host agent has a pending prompt (Elicitation or AskUserQuestion), the chibi sprite SHALL change its emotion to indicate that user input is needed.

#### Scenario: Prompt pending triggers waiting emotion
- **WHEN** the host receives an Elicitation or AskUserQuestion and forwards it to the device
- **THEN** the device updates the chibi sprite emotion to "waiting"
- **AND** the sprite displays a bubble with text "Need input"

#### Scenario: Terminal prompt response clears sprite reminder
- **WHEN** the user responds to the prompt through the terminal
- **THEN** the host sends an updated snapshot to the device
- **AND** the device updates the sprite emotion back to "idle"
- **AND** the bubble disappears

#### Scenario: Approval and prompt both pending
- **WHEN** both an approval and a prompt are pending simultaneously
- **THEN** the sprite displays the "thinking" emotion (approval takes precedence)
- **AND** the bubble text shows "Need permission"

### Requirement: Device SHALL NOT display approval overlay
The ESP32 device SHALL NOT display the full-screen approval overlay with Accept/Decline/Always buttons.

#### Scenario: Approval request arrives on device
- **WHEN** the device receives a `claude.approval.request` event
- **THEN** the device stores the pending approval state internally
- **AND** the device updates the sprite emotion
- **AND** the device does NOT show the approval overlay

### Requirement: Device SHALL NOT display prompt overlay
The ESP32 device SHALL NOT display the prompt overlay with option buttons.

#### Scenario: Prompt request arrives on device
- **WHEN** the device receives a `claude.prompt.request` event
- **THEN** the device stores the pending prompt state internally
- **AND** the device updates the sprite emotion
- **AND** the device does NOT show the prompt overlay
