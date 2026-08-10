# Task

Current + next task only. Completed work is purged from here into
`CHANGELOG.md` as it finishes — this file should stay short. Architecture
knowledge and known issues belong in `DECISIONS.md`, not here.

## Current task

**Release-prep strip-down done.** All debug/diagnostic scaffolding built
up during hardware bring-up (the `WolTraceStage` trace ring, the
`WOL_DEBUG_STATUS` report, the companion app's "Debug Target" Ping/WOL
Test UI row and its log file) has been removed — see `CHANGELOG.md`'s
2026-08-10 "Release prep" entry and `DECISIONS.md` for the full writeup.
Both firmware targets rebuild clean with zero leftover debug symbols;
companion typecheck + full test suite (279/279) pass. What remains is
exactly the shippable "Wake-on-LAN" feature (UI section + automatic-
trigger/host-alive-gate/resend logic).

- [ ] Real hardware smoke test after the strip-down: controller connect
      while PC is on → no WOL (still skips); controller connect while PC
      is off → WOL fires and wakes the PC. Confirms nothing load-bearing
      was accidentally removed alongside the debug code (not yet run —
      the prior end-to-end hardware confirmation predates this pass).

## Next task

**Core WOL feature confirmed working end-to-end on real hardware**
(2026-08-10): controller connects while the target PC is off, WOL fires
and gets ARP-confirmed within ~10s, the PC wakes automatically, and the
controller stays connected through the whole boot with no disconnect. The
host-alive gate (skip WOL when the PC is already on) is also confirmed
working on real hardware. Phase 7's smoke test is done.

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
  end-to-end success is the headline result). Review the whole diff for
  anything unrelated to WOL before opening the PR.
- Once Phase 9/10 are done, open the PR against `upstream/port-dev`.
- Longer-running/soak testing (WOL across many PC on/off cycles, different
  network conditions) is optional polish, not a blocker — the core
  end-to-end path is proven.
