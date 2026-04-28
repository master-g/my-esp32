## ADDED Requirements

### Requirement: Device prompt buttons SHALL display option text
When a prompt with options is displayed on the ESP32 device, each button MUST show the corresponding option text (truncated if necessary), not just a sequence number.

#### Scenario: Prompt with options shows text on buttons
- **WHEN** a prompt with 3 options ("Buy", "Sell", "Hold") is displayed
- **THEN** the device shows 3 buttons labeled "Buy", "Sell", "Hold"
- **AND** NOT buttons labeled "1", "2", "3"

#### Scenario: Long option text is truncated
- **WHEN** a prompt option text exceeds 10 characters
- **THEN** the button label is truncated with ellipsis (e.g., "Buy more…")
- **AND** the full option text is visible in the prompt description area above the buttons

#### Scenario: Prompt with no options shows hint
- **WHEN** a prompt has zero options
- **THEN** no option buttons are displayed
- **AND** a hint message is shown (e.g., "Answer in terminal")
