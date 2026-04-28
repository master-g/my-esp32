## MODIFIED Requirements

### Requirement: Device approval overlay SHALL continue to display
When an approval request (PermissionRequest) arrives on the ESP32 device, the device SHALL display the full-screen approval overlay with Accept/Decline/Always buttons.

#### Scenario: Approval request shows overlay
- **WHEN** the device receives a `claude.approval.request` event
- **THEN** the device stores the pending approval state internally
- **AND** the device displays the approval overlay
- **AND** the overlay shows the tool name and command summary

#### Scenario: User taps Accept on device
- **WHEN** the user taps the Accept button on the approval overlay
- **THEN** the device sends `claude.approval.resolved` with decision "allow"
- **AND** the device hides the overlay

#### Scenario: User taps Decline on device
- **WHEN** the user taps the Decline button on the approval overlay
- **THEN** the device sends `claude.approval.resolved` with decision "deny"
- **AND** the device hides the overlay

#### Scenario: Approval dismissed from host
- **WHEN** the host sends `claude.approval.dismiss`
- **THEN** the device hides the approval overlay

## REMOVED Requirements

### Requirement: Device prompt overlay SHALL display option buttons
**Reason**: Prompt overlay removed; terminal is the primary interaction channel for prompts.
**Migration**: Users answer prompts in the terminal. Device shows sprite reminder only.

#### Scenario: Prompt with options shows buttons
- **REMOVED** This scenario is no longer applicable.

#### Scenario: User selects option on device
- **REMOVED** This scenario is no longer applicable.

#### Scenario: Prompt with no options shows hint
- **REMOVED** This scenario is no longer applicable.

## ADDED Requirements

### Requirement: Device SHALL track prompt state internally
The device SHALL maintain internal state for pending prompts, even though it does not display an overlay.

#### Scenario: Prompt request stored internally
- **WHEN** the device receives a prompt-related event
- **THEN** the device stores the prompt details in internal state
- **AND** the device makes this state available for sprite status updates

#### Scenario: Prompt dismissed clears internal state
- **WHEN** the device receives a prompt dismiss event
- **THEN** the device clears the internal prompt state
- **AND** the sprite reminder is removed
