## ADDED Requirements

### Requirement: Chibi CLI supports testing sprite reminder for approvals
The `chibi` command SHALL provide an `approval` subcommand that sends a test approval request to the ESP32 device to trigger the sprite reminder state.

#### Scenario: User tests approval reminder
- **WHEN** the user runs `esp32dash chibi approval --tool "Bash" --input "rm -rf /tmp/test"`
- **THEN** the command sends a `claude.approval.request` event to the device via direct serial
- **AND** the device updates the sprite to "thinking" emotion with "Need permission" bubble
- **AND** the command prints confirmation and exits immediately (does not block)

#### Scenario: User clears approval reminder
- **WHEN** the user runs `esp32dash chibi approval --clear`
- **THEN** the command sends a `claude.approval.dismiss` event to the device
- **AND** the device clears the pending approval state
- **AND** the sprite returns to "idle" emotion

### Requirement: Chibi CLI supports testing sprite reminder for prompts
The `chibi` command SHALL provide a `prompt` subcommand that sends a test prompt request to the ESP32 device to trigger the sprite reminder state.

#### Scenario: User tests prompt reminder
- **WHEN** the user runs `esp32dash chibi prompt --options "Buy,Sell,Hold" --question "Choose action"`
- **THEN** the command sends a `claude.prompt.request` event to the device via direct serial
- **AND** the device updates the sprite to "waiting" emotion with "Need input" bubble
- **AND** the command prints confirmation and exits immediately

#### Scenario: User clears prompt reminder
- **WHEN** the user runs `esp32dash chibi prompt --clear`
- **THEN** the command sends a `claude.prompt.dismiss` event to the device
- **AND** the device clears the pending prompt state
- **AND** the sprite returns to "idle" emotion
