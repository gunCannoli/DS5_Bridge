# Task

Current + next task only. Completed work is purged from here into
`CHANGELOG.md` as it finishes — this file should stay short. Architecture
knowledge and known issues belong in `DECISIONS.md`, not here.

## Current task

A real test (no reboot this time, USB confirmed solidly connected the
whole time) proved the host-alive gate is checking the wrong signal: our
USB persona is session-scoped (only mounts after a controller-type
handshake that runs *after* the gate check), unlike the reference
implementations' always-enumerated persona their design assumes. User
corrected the approach: event-driven observation window (matching
`awalol/DS5Dongle#207`/`DevFreezing/DS5Dongle-WoL`'s `Observe` state), not
a timer heuristic, and default-fires-WOL unless the host is positively
observed. Measure real timing before picking window/sustain values rather
than guessing. Added (this change, measurement-only, no behavior change
yet): `ConnControllerTypeIdentified` trace stage, elapsed ms from the
`Ready` transition to the controller-type handshake completing. See
`CHANGELOG.md` for the full writeup. Firmware/companion rebuilt.

Also still open from the prior round: the board-transport-recovery-reboot
instrumentation (`BoardTransportRecoveryReboot`/`ConnDisconnectRetrySent`)
hasn't been exercised by a real test yet either — both this and the new
measurement trace need the *same* next real test to read.

- [ ] **Real test with the PC on** (so the controller-type handshake
      actually completes and `conn-controller-type-identified` fires).
      Read `ds5bridge-wol-debug.log` for its `detail` value(s) — this is
      the real elapsed-ms data needed to design the `Observe`-equivalent
      state's window/sustain thresholds next.
- [ ] Same test: check for `board-transport-recovery-reboot`/
      `conn-disconnect-retry-sent` (still unexplained from the prior
      round) and confirm/deny whether it recurs.
- [ ] Once real timing data is in hand: design and implement the actual
      `Observe`-equivalent `WifiState` (bounded window, sustained-active
      confirm threshold, default-fires-unless-confirmed-active) to replace
      the current buggy instant `usb_host_active()` check in
      `wolwifi_on_controller_connect()`.
- [ ] **Real PC-off retest**: confirm WOL still fires normally when the PC
      is genuinely off — the fifteenth-bug-fixed end-to-end path must stay
      unaffected by all of this.
- [ ] `-DWOL_ALWAYS=ON` escape hatch: not urgent, only matters if a real
      board hits the known USB-stays-active-in-S5 limitation.

## Next task

**Core WOL feature already confirmed working end-to-end on real hardware**
(2026-08-10, before the host-alive gate was added): controller connects
while the target PC is off, WOL fires and gets ARP-confirmed within ~10s,
the PC wakes automatically, and the controller stays connected through the
whole boot with no disconnect. Phase 7's smoke test is done.

- Phase 9 — failure-behavior verification: confirm Wi-Fi/WOL failure never
  blocks or delays BT/controller init or normal operation (non-blocking by
  construction; needs a runtime check — e.g. WOL disabled/misconfigured, or
  Wi-Fi genuinely unreachable, shouldn't affect controller connect/input
  latency at all).
- Phase 10 — PR prep: commits already reasonably separated by feature area
  (firmware Wi-Fi/WOL, companion app config, build integration) — review
  the branch's full commit log for anything to squash/reorder before
  opening the PR. Write the PR description (why WOL, how it works,
  supported board, config requirements, test results — the real
  end-to-end success is the headline result). Decide whether the debug
  Ping/WOL Test tooling ships as part of the feature or stays
  internal-only (see `DECISIONS.md`'s closing note — leaning toward
  shipping it, given how many real bugs it caught). Review the whole diff
  for anything unrelated to WOL before opening the PR.
- Once Phase 9/10 are done, open the PR against `upstream/port-dev`.
- Longer-running/soak testing (WOL across many PC on/off cycles, different
  network conditions) is optional polish, not a blocker — the core
  end-to-end path is proven.
