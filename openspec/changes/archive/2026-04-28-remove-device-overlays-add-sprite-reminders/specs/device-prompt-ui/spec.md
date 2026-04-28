## REMOVED Requirements

### Requirement: Device prompt buttons SHALL display option text
**Reason**: Device no longer displays prompt overlay; interaction happens in terminal.
**Migration**: Prompt options are displayed in the terminal by Claude Code's native UI.

#### Scenario: Prompt with options shows text on buttons
- **REMOVED** This scenario is no longer applicable.

#### Scenario: Long option text is truncated
- **REMOVED** This scenario is no longer applicable.

#### Scenario: Prompt with no options shows hint
- **REMOVED** This scenario is no longer applicable.

### Requirement: Device approval overlay with Accept/Decline/Always buttons
**Reason**: Device no longer displays approval overlay; interaction happens in terminal.
**Migration**: Approval decisions are made in the terminal by Claude Code's native UI.

#### Scenario: Approval overlay displayed
- **REMOVED** This scenario is no longer applicable.

#### Scenario: User taps Accept button
- **REMOVED** This scenario is no longer applicable.

#### Scenario: User taps Decline button
- **REMOVED** This scenario is no longer applicable.

## ADDED Requirements

### Requirement: Device SHALL track pending state internally
The device SHALL maintain internal state for pending approvals and prompts, even though it does not display overlays.

#### Scenario: Approval request stored internally
- **WHEN** the device receives a `claude.approval.request` event
- **THEN** the device stores the approval details (tool_name, description, id) in internal state
- **AND** the device makes this state available for sprite status updates

#### Scenario: Prompt request stored internally
- **WHEN** the device receives a `claude.prompt.request` event
- **THEN** the device stores the prompt details (title, question, options, id) in internal state
- **AND** the device makes this state available for sprite status updates

#### Scenario: Dismiss clears internal state
- **WHEN** the device receives a `claude.approval.dismiss` or `claude.prompt.dismiss` event
- **THEN** the device clears the corresponding internal pending state
- **AND** the sprite reminder is removed
