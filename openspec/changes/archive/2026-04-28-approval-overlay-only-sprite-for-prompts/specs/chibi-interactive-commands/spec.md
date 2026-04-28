## ADDED Requirements

### Requirement: Chibi CLI supports testing approval overlay
The `chibi` command SHALL provide an `approval` subcommand that sends a test approval request to the ESP32 device to trigger the approval overlay.

#### Scenario: User tests approval overlay
- **WHEN** the user runs `esp32dash chibi approval --tool "Bash" --input "rm -rf /tmp/test"`
- **THEN** the command sends a `claude.approval.request` event to the device via direct serial
- **AND** the device displays the approval overlay with Accept/Decline/Always buttons
- **AND** the command blocks and waits for the device response (allow/deny/always)
- **AND** the command prints the decision to stdout and exits with code 0

#### Scenario: User clears approval overlay
- **WHEN** the user runs `esp32dash chibi approval --clear`
- **THEN** the command sends a `claude.approval.dismiss` event to the device
- **AND** the device hides the approval overlay

### Requirement: Chibi CLI supports testing prompt sprite reminder
The `chibi` command SHALL provide a `prompt` subcommand that sends a test prompt request to the ESP32 device to trigger the sprite reminder state.

#### Scenario: User tests prompt reminder
- **WHEN** the user runs `esp32dash chibi prompt --options "Buy,Sell,Hold" --question "Choose action"`
- **THEN** the command sends a `claude.prompt.request` event to the device via direct serial
- **AND** the device updates the sprite to "waiting" emotion with "Need input" bubble
- **AND** the command does NOT block (exits immediately after sending)

#### Scenario: User clears prompt reminder
- **WHEN** the user runs `esp32dash chibi prompt --clear`
- **THEN** the command sends a `claude.prompt.dismiss` event to the device
- **AND** the device clears the pending prompt state
- **AND** the sprite returns to "idle" emotion
