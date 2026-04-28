## ADDED Requirements

### Requirement: PromptStore SHALL expire unresolved prompts after timeout
The `PromptStore` MUST automatically remove prompts that have not been resolved within a configurable timeout period.

#### Scenario: Prompt expires after timeout
- **WHEN** a prompt is submitted to the store and remains unresolved
- **AND** the configured timeout period elapses
- **THEN** the prompt is removed from the store
- **AND** any callers waiting for resolution receive a timeout indication

#### Scenario: Resolved prompt is cleaned up immediately
- **WHEN** a prompt is resolved before the timeout
- **THEN** it is removed from the store immediately
- **AND** no cleanup task attempts to remove it later

#### Scenario: Cleanup task runs periodically
- **WHEN** the PromptStore is initialized
- **THEN** a background task periodically scans for expired prompts
- **AND** removes any prompts whose timeout has elapsed
