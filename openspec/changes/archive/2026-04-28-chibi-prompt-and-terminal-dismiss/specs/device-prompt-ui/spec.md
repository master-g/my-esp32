## MODIFIED Requirements

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

## ADDED Requirements

### Requirement: Approval overlay buttons SHALL be easily tappable
The approval overlay buttons SHALL have adequate size and spacing for touch interaction on the 3.5-inch display.

#### Scenario: Approval buttons are tall enough
- **WHEN** the approval overlay is displayed
- **THEN** each button height SHALL be at least 36 pixels
- **AND** the button text SHALL be readable at the device's viewing distance

#### Scenario: Approval layout is centered and balanced
- **WHEN** the approval overlay is displayed
- **THEN** the tool name and description SHALL be clearly separated from the buttons
- **AND** the button row SHALL be vertically centered in the lower portion of the overlay

### Requirement: Prompt overlay SHALL show option context
The prompt overlay SHALL display the question text prominently and the options in an easy-to-scan layout.

#### Scenario: Prompt question is clearly visible
- **WHEN** a prompt with options is displayed
- **THEN** the question text SHALL use the primary text color
- **AND** the question SHALL be positioned above the option buttons with adequate spacing

#### Scenario: Prompt option buttons are evenly spaced
- **WHEN** a prompt with multiple options is displayed
- **THEN** the option buttons SHALL be evenly distributed horizontally
- **AND** each button SHALL have a minimum width of 60 pixels
