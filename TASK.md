# Task

Current + next task only. Completed work is purged from here into
`CHANGELOG.md` as it finishes — this file should stay short. Architecture
knowledge and known issues belong in `DECISIONS.md`, not here.

## Current task

Bench-testing the host-alive gate surfaced a real, separate issue: the
board is self-rebooting via a deliberate `watchdog_reboot()` call in
`bt.cpp`'s BT disconnect/ACL-pending recovery paths during otherwise-normal
operation, and this was previously invisible in the trace (see
`DECISIONS.md`'s `watchdog_enable_caused_reboot()` entry). Added
`BoardTransportRecoveryReboot`/`ConnDisconnectRetrySent` trace stages to
make it visible instead of guessing further. Firmware/companion build
clean, debug firmware rebuilt at `build/waveshare-debug/ds5-bridge.uf2`.

- [ ] **Real test with the new instrumentation.** Read
      `ds5bridge-wol-debug.log` for: (a) does `board-transport-recovery-reboot`
      appear, and with which detail (0=disconnect-retry-exhausted,
      1=incoming-ACL-pending-timeout, 2=ACL-cancel-incomplete)? (b) does
      `conn-disconnect-retry-sent` show an escalation building up to it
      (1, 2, 3 attempts) or does the reboot happen without any preceding
      retries? (c) once the actual reboot cause is known, diagnose *why*
      that recovery path is firing during normal operation — not yet
      understood.
- [ ] **Re-verify the host-alive gate itself** once reboots aren't
      confusing the picture: with the target PC genuinely on and the board
      NOT freshly rebooted, confirm `wol-trigger-skipped-host-active`
      appears instead of `wol-trigger-fired`, and the lightbar stays dark.
- [ ] **Real PC-off retest**: confirm WOL still fires normally when the PC
      is genuinely off — the fifteenth-bug-fixed end-to-end path must stay
      unaffected by both the gate and this investigation.
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
