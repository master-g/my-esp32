# Pending interaction travels as a level inside claude.update

The device used to learn about a pending interaction from four edge frames (`claude.approval.request/dismiss`, `claude.prompt.request/dismiss`) and kept its own slots, its own copy of the "approval beats prompt" rule and a self-timeout. A lost or filtered dismiss left the device showing something the host had already cleared, and the same state lived in four firmware modules. We now send the one interaction the device should show as a `pending` field (kind + coarse type, or null) in every `claude.update`, and removed the four frames. The device renders whatever the latest snapshot says, so a reconnect or any later update repairs it without bookkeeping on either side.

## Considered Options

- **Keep the four frames, move the device state into `service_claude`.** Smallest change, but dismiss loss and the duplicated preemption rule remain.
- **Merge into one `claude.interaction.request/dismiss` pair.** Fewer frames, same edge-triggered failure modes.
- **Level in the snapshot (chosen).** The host queue only answers "which one now"; the device has no rules to keep in sync.

## Consequences

- The wire protocol changed without a version bump: a new host with old firmware shows no waiting bubble, and old hosts cannot drive new firmware's bubble. Flash and install together.
- Timeouts exist only in the host queue. The device clears the bubble when the bridge goes stale (120 s without transport) instead of running its own timer.
- Home is brought to the foreground only when `pending` goes from null to set. A second interaction arriving while one is already pending does not pull the user back.
