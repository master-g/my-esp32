## ADDED Requirements

### Requirement: Chibi CLI supports interactive prompt testing
The `chibi` command SHALL provide a `prompt` subcommand that sends a test prompt to the ESP32 device and allows the user to select an option from the terminal.

#### Scenario: User initiates prompt test from terminal
- **WHEN** the user runs `esp32dash chibi prompt --options "Buy,Sell,Hold" --question "Choose action"`
- **THEN** the command sends a `claude.prompt.request` event to the device with the specified options
- **AND** the device displays the prompt overlay with the given options

#### Scenario: Terminal selection dismisses device prompt
- **WHEN** the user selects an option in the terminal (via arrow keys or number input)
- **THEN** the command sends the selection to the agent
- **AND** the agent resolves the pending prompt
- **AND** the device receives a `claude.prompt.dismiss` event
- **AND** the device hides the prompt overlay

#### Scenario: Device selection is printed to terminal
- **WHEN** the user selects an option on the device instead of the terminal
- **THEN** the device sends `claude.prompt.response`
- **AND** the command prints the selected option label to stdout
- **AND** the command exits with code 0

### Requirement: Chibi CLI supports interactive approval testing
The `chibi` command SHALL provide an `approval` subcommand that sends a test approval request to the ESP32 device.

#### Scenario: User initiates approval test from terminal
- **WHEN** the user runs `esp32dash chibi approval --tool "Bash" --input "rm -rf /tmp/test"`
- **THEN** the command sends a `claude.approval.request` event to the device
- **AND** the device displays the approval overlay with Accept/Decline/Always buttons

#### Scenario: Terminal decision dismisses device approval
- **WHEN** the user types 'a' (allow), 'd' (deny), or 'y' (always) in the terminal
- **THEN** the command sends the decision to the agent
- **AND** the agent resolves the pending approval
- **AND** the device receives a `claude.approval.dismiss` event
- **AND** the device hides the approval overlay

#### Scenario: Device decision is printed to terminal
- **WHEN** the user taps a button on the device
- **THEN** the device sends `claude.approval.resolved`
- **AND** the command prints the decision to stdout
- **AND** the command exits with code 0
