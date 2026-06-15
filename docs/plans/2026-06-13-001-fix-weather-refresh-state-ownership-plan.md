---
title: "fix: Give the weather worker sole ownership of refresh state"
type: fix
date: 2026-06-13
origin: plans/001-weather-refresh-inprogress-guard.md
---

# fix: Give the weather worker sole ownership of refresh state

## Summary

`weather_service_request_refresh()` writes `s_snapshot.state = WEATHER_REFRESHING`
on the request path, but the `s_refresh_in_progress` flag and the `REFRESHING`
state transition are actually owned by the worker task (`weather_task`), which
also uses the flag to gate whether it runs a fetch. The request path writing
state — but not participating in the flag — produces a snapshot where
`state == WEATHER_REFRESHING` while `s_refresh_in_progress == false`, and leaves
the state stuck in `REFRESHING` if the queued command is never serviced.

The fix moves all `REFRESHING`/`s_refresh_in_progress` ownership to the worker:
`weather_service_request_refresh()` keeps only its guard (in-progress short-circuit
+ debounce window) and the enqueue, dropping the state write and the now-redundant
publish. Scope is a single function in one file. Priority P1, risk LOW.

This plan supersedes the fix direction in the origin document — see Problem Frame.

---

## Problem Frame

`s_refresh_in_progress` is worker-owned. In `src/components/service_weather/src/service_weather.c`
the worker loop (`weather_task`) is the only correct writer of both the flag and
the `REFRESHING` state:

- Under the mutex it reads the flag; only if it is `false` does it set
  `s_refresh_in_progress = true`, set `state = WEATHER_REFRESHING` **conditionally**
  (`s_last_success_us > 0`), and copy the location config.
- It then fetches and, on both success and failure paths, clears
  `s_refresh_in_progress = false` and sets a terminal state (`LIVE`/`STALE`/`ERROR`).
- The flag doubles as a fetch gate: if it is already `true` when a command is
  dequeued, the worker skips the fetch.

`weather_service_request_refresh()` is the refresh-request entry. Its only live
caller is `weather_service_apply_location_config()` (`service_weather.c:398`),
reached when the host pushes a weather-location config through `device_link`
(`device_link.c:866`) — there is no UI tap or other manual trigger wired to it.
It takes the mutex, short-circuits on `s_refresh_in_progress` or the debounce
window, then **unconditionally** sets `s_last_request_us`, sets
`state = WEATHER_REFRESHING`, releases, publishes, and enqueues a
`WEATHER_CMD_REFRESH`. It never touches the flag. Note that the sole caller zeroes
the snapshot (`state = EMPTY`, `s_last_success_us = 0`) immediately before
requesting. Two consequences:

1. **Inconsistent snapshot / intent mismatch.** The worker deliberately sets
   `REFRESHING` only when `s_last_success_us > 0` (first-ever load stays
   `EMPTY`/`ERROR` rather than showing a misleading "refreshing"). The request path
   ignores that condition, so a consumer can observe `REFRESHING` with
   `s_refresh_in_progress == false`, contradicting the flag's meaning.
2. **Stuck-in-REFRESHING window.** If the worker never services the command —
   queue full (the `xQueueSend` return is discarded) or the link dropped between
   request and dequeue — the state set by the request path stays `REFRESHING`
   until some later refresh re-runs.

**Why the origin fix is wrong.** The origin document
(`plans/001-weather-refresh-inprogress-guard.md`) proposes setting
`s_refresh_in_progress = true` inside `weather_service_request_refresh()`. Because
the worker uses that flag as its fetch gate, pre-setting it makes the worker read
`true` on dequeue and skip the fetch entirely — the flag is then never cleared and
the state is **permanently** wedged in `REFRESHING`, rejecting all subsequent
refreshes. That trades a transient UI inconsistency for a hard deadlock. The origin
plan inspected only `weather_service_request_refresh()` and did not account for how
the worker consumes the flag.

**Chosen direction:** the worker already owns the flag and the `REFRESHING`
transition atomically within one execution path (it sets `REFRESHING` only on the
same path that is guaranteed to reach the clearing code). Let it keep sole
ownership; remove the competing writer.

---

## Requirements

- **R1.** `weather_service_request_refresh()` must not produce a snapshot where
  `state == WEATHER_REFRESHING` while `s_refresh_in_progress == false`.
- **R2.** The request path (`weather_service_request_refresh`) writes neither
  `s_refresh_in_progress` nor `WEATHER_REFRESHING`. The `WEATHER_REFRESHING`
  transition and the in-progress flag's `true`-set stay with `weather_task`. (Flag
  *clearing* to `false` also runs on the init/reset path in `apply_location_config`,
  a separate writer this fix does not consolidate — see Deferred to follow-up. R2 is
  scoped to what removing the request-path write establishes, not to full
  single-owner clearing.)
- **R3.** The refresh-request guard behavior is preserved: an in-progress refresh
  short-circuits, and the `WEATHER_MANUAL_REFRESH_GUARD_US` debounce window on
  `s_last_request_us` still suppresses rapid repeat requests.
- **R4.** When the link is disconnected or `weather_refresh_allowed` is false,
  `weather_service_request_refresh()` still returns early without enqueuing
  (behavior unchanged).

---

## Key Technical Decisions

- **KTD1 — Worker holds sole ownership of in-progress / REFRESHING state.**
  Rationale: the flag gates the fetch and is cleared on every fetch exit path, so
  the only writer that can keep flag and state consistent is the worker. Any write
  from the request path either lies (state without flag) or deadlocks (flag without
  fetch). Rejected alternative: mirror the worker's `s_last_success_us > 0`
  condition in the request path and roll back state on `xQueueSend` failure — it
  works but keeps two writers of `REFRESHING` and re-introduces the consistency
  burden this fix removes.
- **KTD2 — Drop the `publish_weather_event()` call in the request path.** Once the
  request path no longer mutates the snapshot, that publish carries no new state;
  the worker already publishes when it begins the fetch. Removing it also keeps the
  function clear of the "publish outside the service mutex" concern entirely
  (see project constraint in `CLAUDE.md`). On the live config-change path the
  worker's begin-fetch publish carries `EMPTY`, not `REFRESHING` (because
  `s_last_success_us == 0` after the reset) — this is the intended first-load
  behavior, not a lost refresh indication.

---

## Implementation Units

### U1. Reduce `weather_service_request_refresh()` to guard + enqueue

- **Goal:** Remove the request path's writes to `s_snapshot.state` and its publish,
  leaving only the connectivity/policy check, the in-progress + debounce guard, the
  `s_last_request_us` stamp, and the enqueue. Refresh state becomes worker-owned.
- **Requirements:** R1, R2, R3, R4.
- **Dependencies:** none.
- **Files:**
  - `src/components/service_weather/src/service_weather.c` — `weather_service_request_refresh()` only.
  - No test file: the firmware has no C test suite (see `CLAUDE.md` — verification
    is `make build` plus on-device smoke; there is no firmware test harness to add to).
- **Approach:**
  - Keep the early `return` on `!net_manager_is_connected() || !policy.weather_refresh_allowed`.
  - Keep the mutex-guarded short-circuit on `s_refresh_in_progress` or the
    `WEATHER_MANUAL_REFRESH_GUARD_US` window.
  - Keep `s_last_request_us = now_us` under the mutex.
  - Remove `s_snapshot.state = WEATHER_REFRESHING` and the `publish_weather_event()` call.
  - Keep `xQueueSend(s_command_queue, &cmd, 0)` after releasing the mutex.
  - Net effect: the worker's existing block (set flag → conditional `REFRESHING` →
    fetch → clear flag + terminal state) is the only place that touches refresh state.
- **Patterns to follow:**
  - Static state guarded by `s_mutex` with `xSemaphoreTake(..., portMAX_DELAY)` /
    `xSemaphoreGive(...)`, matching the rest of `service_weather.c`.
  - The worker's own ownership block in `weather_task` (the conditional
    `s_last_success_us > 0` REFRESHING set) is the canonical shape to defer to.
  - State transitions logged with `ESP_LOGI` / `ESP_LOGW` where the worker already does;
    the request path adds no new transition to log.
- **Test scenarios:** No firmware C test harness exists, so these are on-device
  smoke checks plus a code-review invariant assertion. The only live trigger is a
  host weather-location config push (`weather_service_apply_location_config`); there
  is no UI manual-refresh action, so reproduce the on-device cases by pushing a
  location config from the host; the *Debounce* case is a code-review check (marked
  inline). (`WEATHER_MANUAL_REFRESH_GUARD_US` is a codebase-inherited constant name;
  it debounces all refresh requests, not a UI-manual action.) Each names
  input → action → expected:
  - *Happy path (prior success):* device has fetched weather at least once
    (`s_last_success_us > 0`), then a config push requests a refresh → the worker
    sets `REFRESHING`, completes, returns to `LIVE`; no observable window of
    `REFRESHING` with the flag clear.
  - *First-ever / post-config-change:* `apply_location_config` zeroes the snapshot
    (`state = EMPTY`, `s_last_success_us = 0`) before requesting → the request path
    no longer writes `REFRESHING`, and the worker keeps `EMPTY` (its
    `s_last_success_us > 0` gate is false) until the fetch succeeds. First boot and
    every later config change coincide on this path.
  - *Debounce (code-review check, not reproducible via the live caller):*
    `apply_location_config` zeroes `s_last_request_us` before each request, so two
    config pushes never exercise the debounce guard. Verify by reading
    `request_refresh` in isolation — two calls within `WEATHER_MANUAL_REFRESH_GUARD_US`
    with a non-zero `s_last_request_us` short-circuit the second (R3).
  - *In-progress:* a config push while a worker fetch is running → the request
    short-circuits on `s_refresh_in_progress` (R3).
  - *Disconnected:* link down → request → early return, nothing enqueued, state
    unchanged (R4).
  - *Stuck-state regression:* with the request-path state write removed, confirm no
    path outside the worker can leave the snapshot in `REFRESHING` — e.g. a queue-full
    / unserviced-command path leaves the state untouched rather than latched in
    `REFRESHING` (R1, R2). Note: the pre-enqueue `s_last_request_us` stamp is unchanged,
    so a dropped command debounces retries only for callers that do not reset it — but
    the sole live caller (`apply_location_config`) zeroes `s_last_request_us` before
    each request, so its dropped-command retries are not debounced. Pre-existing
    behavior this fix does not address.
  - *Invariant assertion (review):* after the change, no dynamic refresh path outside
    `weather_task` writes `WEATHER_REFRESHING` or sets `s_refresh_in_progress = true`
    (the flag is cleared to `false` by the worker on both fetch-exit paths and on the
    init/reset paths in `weather_service_init` / `apply_location_config`; the
    load-bearing invariant is that only the worker sets it `true` and only the worker
    writes `WEATHER_REFRESHING`). grep-level check on `service_weather.c`.
- **Verification:** `make build` exits 0; the config-push and disconnected smoke
  checks above behave as described on-device; a grep over `service_weather.c`
  confirms `WEATHER_REFRESHING` writes and `s_refresh_in_progress = true` occur only
  in `weather_task`.

---

## Scope Boundaries

**In scope:**
- `weather_service_request_refresh()` in `src/components/service_weather/src/service_weather.c`.

**Out of scope:**
- `weather_task` (the worker) — already the correct owner; not modified.
- `weather_client.c`, `weather_mapper.c` — transport and mapping unchanged.
- Other services (`service_time`, `service_settings`, etc.).
- App/UI labels or layout — consumers keep reading `WEATHER_REFRESHING`; this fix
  only changes *who* sets it, not the enum or its meaning.

**Deferred to follow-up (out of scope here):**
- `weather_service_request_refresh()` is currently unreachable from the device UI —
  only a host weather-location config push triggers it. Whether to wire a UI refresh
  affordance is a separate product decision.
- `weather_service_apply_location_config()` resets `s_refresh_in_progress = false`
  outside the worker; if a fetch is in flight during a location change, that reset
  races the worker's ownership of the flag. Worth a separate concurrency audit;
  neither introduced nor addressed by this fix.

**Note on origin verification commands.** The origin document lists a Rust test
command (`cargo test --manifest-path tools/esp32dash/Cargo.toml`) as a success
check. That is not load-bearing for this change: the bug is in firmware C, and the
`esp32dash` host-agent Rust tests do not exercise `service_weather`. The real gate
is `make build` plus the on-device smoke checks in U1.

---

## Pre-Implementation Drift Check

Before editing, confirm the live code still matches this plan's premises (it was
read at commit `5c3a006`):

- `weather_service_request_refresh()` still unconditionally sets
  `s_snapshot.state = WEATHER_REFRESHING` and calls `publish_weather_event()`.
- `weather_task` still owns `s_refresh_in_progress` (sets `true` before fetch, clears
  on both exit paths) and gates the fetch on it.

If either premise has changed, stop and re-evaluate — the fix direction depends on
the worker remaining the flag's sole owner.
