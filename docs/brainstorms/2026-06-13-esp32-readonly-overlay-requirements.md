---
date: 2026-06-13
topic: esp32-readonly-overlay
---

# ESP32 Claude Overlay → Read-Only Status Display

## Summary

Stop the ESP32 from being able to authorize Claude in any way. All three
device-decision paths — tool permission, Elicitation, and AskUserQuestion — lose
their on-device decision capability; every authorization returns to the Mac's
native prompt. All waiting-class interactions show the same read-only overlay with
their type. The device is demoted from an authorization decision point to a status
notifier.

---

## Problem Frame

Today the device can decide, and not only for tool permissions. The host exposes
three structurally identical authorization paths, all of which let a tap on the
ESP32 shape what Claude Code does:

- **Tool permission** (`handle_permission_request`) — the device returns
  allow / deny / allow_always; allow_always can permanently whitelist a tool.
- **Elicitation** (`handle_elicitation`) — the device's selected option becomes the
  accepted input value returned to Claude.
- **AskUserQuestion / PreToolUse** (`handle_ask_user_question`) — the device's
  selection becomes the tool's answer, or denies it.

All three solicit a decision over serial and feed it back to Claude Code through
two device→host authorization frames: `claude.approval.resolved` (permission) and
`claude.prompt.response` (Elicitation and AskUserQuestion). Only the permission path
is currently wired to the blocking hook; Elicitation and AskUserQuestion are latent
(their code is live, but the installed hook script does not route them to the
device today). The latency is fragile — wiring either one later silently reopens
the bypass.

The device holding any decision authority is what lets a locked Mac be bypassed,
forces request types the device cannot fully render to be answered on a small
screen, and requires the blocking poll, the two response frames, and the approval
generation check to all exist. The cost shape is a security hole plus a standing
maintenance burden, both rooted in letting an always-on external screen authorize
actions.

---

## Key Decisions

- **KD1. One principle, swept across every path.** Any path where the device can
  return a decision that shapes Claude's behavior (approve / deny / accept / select)
  loses that capability and becomes read-only; the decision returns to the Mac. This
  is the security-driven rule that classifies every interaction type uniformly.
- **KD2. Close the latent paths too.** Elicitation and AskUserQuestion aren't an
  active bypass today, but their decision capability is removed anyway so a future
  hook-wire can't reopen the channel. Defensive, not speculative — the code is live.
- **KD3. Decisions return to the Mac's native prompt.** Reuse the host's existing
  "let Claude Code's native dialog decide" fall-through as the default for all three
  paths. A locked Mac withholds that prompt — that is the security property.
- **KD4. All waiting types share one read-only overlay.** Permission, Elicitation,
  and AskUserQuestion all drive the same overlay showing the interaction's type.
  This delivers the unified-display intent; non-tool types need a new "request
  pending" signal to reach the overlay instead of only sprite emotion.
- **KD5. Show type, not content.** The overlay shows a coarse category; no command,
  argument, or option content crosses to the always-on screen.
- **KD6. Ambient dismissal.** Show on request; clear on later activity plus a
  timeout. No precise per-request resolution tracking once the device doesn't decide.
- **KD7. Sprite emotion is retained, overlay is the primary wait display.** The
  sprite emotion stays as an additional expressive layer; the read-only overlay is
  the authoritative "Claude is waiting" surface. Exact layering is a planning detail.

---

## Actors

- **A1. User (developer at the Mac)** — makes every authorization decision on the
  Mac's native prompt, for all interaction types.
- **A2. ESP32 device** — displays that Claude is waiting and the interaction type;
  holds no decision power on any path.
- **A3. esp32dash host agent** — wraps the Claude Code hooks, pushes status to the
  device, and lets the native prompt resolve every decision.
- **A4. Claude Code** — source of permission, Elicitation, and AskUserQuestion
  requests; its native prompt is the sole decision point.

---

## Requirements

**ESP32 behavior**

- R1. The overlay is read-only: no buttons, no touch-driven decision, no options to
  select, for any interaction type.
- R2. The overlay shows the interaction's coarse type (e.g. "Bash command",
  "Question"), never command / argument / option content.
- R3. The overlay appears on a "request pending" signal and clears on a later
  activity signal with a timeout fallback; it does not track each request's outcome.
- R4. Permission, Elicitation, and AskUserQuestion all collapse to the same waiting
  overlay, with no per-type branching that gives one of them decision power.

**Decision path**

- R5. The device sends no decision back to the host on any path — both
  `claude.approval.resolved` and `claude.prompt.response` inbound frames are removed.
- R6. Tool-permission, Elicitation, and AskUserQuestion decisions are all made on the
  Mac's native prompt.
- R7. While the Mac is locked, no interaction of any type can be authorized via the
  ESP32.
- R8. The latent Elicitation and AskUserQuestion device-decision paths are removed,
  not merely left unwired, so they cannot be reactivated as a bypass.

**Host and protocol**

- R9. The blocking respond / poll architecture in all three handlers
  (`handle_permission_request`, `handle_elicitation`, `handle_ask_user_question`) is
  replaced with a host-only decision source (native prompt); the host pushes a
  one-way "pending" notification and does not block on the device.
- R10. Both inbound authorization frames are removed from the device_link protocol;
  the one-way request/dismiss notifications and the unrelated `protocol.error`
  framing guard stay.

**Documentation**

- R11. CLAUDE.md is updated: approvals (all types) are one-way status notifications;
  the device returns no decision; the approval generation check is removed with the
  decision RPC while `protocol.error` remains load-bearing.

---

## Acceptance Examples

- AE1. **Covers R1, R2, R6.** A tool request arrives → the ESP32 shows "Claude is
  waiting · Bash command" and the Mac shows its native prompt → the user decides on
  the Mac (including allow_always) → a later activity signal clears the overlay.
- AE2. **Covers R7.** Any request arrives while the Mac is locked → the ESP32 shows
  the status with no way to authorize on the device; the native prompt is behind the
  lock screen → the user must unlock to decide.
- AE3. **Covers R4, R6.** An Elicitation or AskUserQuestion arrives → the ESP32 shows
  the same read-only overlay (type only, no options) → the selection happens on the
  Mac, not the device.
- AE4. **Covers R3.** The user declines on the Mac (no tool runs, so no PostToolUse
  fires) → the overlay clears on a conversation-stop signal or the timeout.
- AE5. **Covers R5.** Across every path the ESP32 emits no frame carrying a decision
  — neither `claude.approval.resolved` nor `claude.prompt.response`.
- AE6. **Covers R8.** Even if the Elicitation / AskUserQuestion hooks were wired to
  the device after this change, no device decision can be returned — the inbound
  frame handlers and the device-decision logic are gone.

---

## Scope Boundaries

In scope: removing device decision authority across all three authorization paths,
unifying the read-only overlay across interaction types, and the host/protocol
simplification that follows. Out of scope:

- Selective downgrade (read-only only when locked) — a single read-only mode (KD1).
- Showing full command / argument / option content on the device (R2).
- Removing sprite emotion — it is retained as an additional expressive layer (KD7);
  the overlay is added as the primary wait display.
- `protocol.error` and the serial line-overflow guard — unrelated to approvals; stays.

### Deferred to Follow-Up Work

- **Audit-branch coordination.** The audit branch's completed U10–U13
  (approval-input reliability) patch a path this refactor removes; once this lands
  they are superseded — treat those commits as superseded rather than merging then
  deleting. The audit branch's non-approval work is independent.
- **Branch.** This refactor is independent of `fix/weather-refresh-state-ownership`
  and should land on its own branch.
- **Weather-refresh race fix** — separate and unrelated.

---

## Dependencies / Assumptions

- A1 (assumption). The user runs Claude Code interactively, so requests surface on a
  native prompt that the lock screen withholds. A headless or
  `--permission-mode=bypassPermissions` run has no native prompt; the security
  property does not apply there, and that configuration is out of scope.
- A2 (assumption, confirm during planning). Claude Code emits later hook signals
  (PostToolUse / Stop) sufficient to drive overlay dismissal; otherwise dismissal
  falls back to the timeout alone.

---

## Outstanding Questions (Deferred to Planning)

Carried from the brainstorm:

- The overlay-dismiss timeout value and whether `Stop` is a good enough deny-path
  signal in practice.
- The coarse type → label mapping for each interaction type and the generic fallback.
- The residual shape of the one-way request/dismiss notifications after the decision
  frames are removed.

Surfaced by the plan-stage review (HOW-level, for ce-plan to resolve):

- **Device self-timeout.** Removing the decision RPC also removes the device-side
  overlay timeout; decide whether to keep a lightweight device-side self-timeout or
  accept that a wedged-but-connected host can leave the overlay stuck.
- **Overlay supersede.** Today both ends block a second pending request while one is
  visible; the unified overlay needs real supersede (dismiss-then-show) or overlapping
  requests display a stale type.
- **Hook routing for the notify.** The non-blocking notify must still POST the
  pending request so the device gets a frame — a plain `ingest` reroute would skip
  that. Settle the exact host path.
- **Hook semantics.** Verify that a hook returning empty stdout (no decision) makes
  Claude Code show its native prompt rather than defaulting to deny — load-bearing
  for the whole design, and only verified today on the agent-unreachable branch.
- **Existing dismiss machinery.** PostToolUse / Stop already drive dismissal on the
  host; the real open question is whether `Stop` fires on a Mac-side deny, not new
  wiring.
- **Deploy atomicity.** The host-side frame removal and the device-side frame removal
  should land together so no partial-deploy window leaves a decision channel open.
