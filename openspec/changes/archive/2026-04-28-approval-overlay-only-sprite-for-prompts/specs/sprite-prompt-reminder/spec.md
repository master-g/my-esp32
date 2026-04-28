## ADDED Requirements

### Requirement: Sprite SHALL display reminder for pending prompts
When the host agent has a pending prompt (Elicitation or AskUserQuestion), the chibi sprite SHALL change its emotion to indicate that user input is needed.

#### Scenario: Prompt pending triggers waiting emotion
- **WHEN** the host receives an Elicitation or AskUserQuestion
- **THEN** the device updates the chibi sprite emotion to "waiting"
- **AND** the sprite displays a bubble with text "Need input"

#### Scenario: Terminal prompt response clears sprite reminder
- **WHEN** the user responds to the prompt through the terminal
- **THEN** the host sends an updated snapshot with `has_pending_prompt: false`
- **AND** the device updates the sprite emotion back to "idle"
- **AND** the bubble disappears

#### Scenario: Multiple prompts pending
- **WHEN** multiple prompts are pending simultaneously
- **THEN** the sprite maintains the "waiting" emotion
- **AND** the sprite state does not flicker

### Requirement: Sprite reminder takes lower priority than approval overlay
When both an approval and a prompt are pending, the device SHALL display the approval overlay and the sprite SHALL show the approval-related state.

#### Scenario: Approval and prompt both pending
- **WHEN** both an approval and a prompt are pending simultaneously
- **THEN** the device displays the approval overlay (Accept/Decline/Always buttons)
- **AND** the sprite displays "thinking" emotion with "Need permission" bubble
- **AND** the prompt reminder is suppressed until the approval is resolved

### Requirement: Device SHALL NOT display prompt overlay
The ESP32 device SHALL NOT display the full-screen prompt overlay with option buttons.

#### Scenario: Prompt request arrives on device
- **WHEN** the device receives a prompt-related event (Elicitation or AskUserQuestion)
- **THEN** the device stores the pending prompt state internally
- **AND** the device updates the sprite emotion to "waiting"
- **AND** the device does NOT show the prompt overlay
