# Task

Current + next task only. Completed work is purged from here into
`CHANGELOG.md` as it finishes — this file should stay short. Architecture
knowledge and known issues belong in `DECISIONS.md`, not here.

## Current task

Real measurement came back (23ms clean handshake time) and the actual fix
is implemented: `ObserveHost` mini-state-machine replacing the buggy
instant `usb_host_active()` check, `WOL_OBSERVE_HOST_WINDOW_MS = 2000` /
`WOL_OBSERVE_HOST_SUSTAIN_MS = 100`, default-fires-WOL-unless-host-observed-
active. See `DECISIONS.md` for the full corrected design writeup. Firmware
compiles clean (both `WOL_ALWAYS` on/off); no companion changes needed.
Debug firmware rebuilt at `build/waveshare-debug/ds5-bridge.uf2`.

Still open from two rounds ago: the board-transport-recovery-reboot
instrumentation (`BoardTransportRecoveryReboot`/`ConnDisconnectRetrySent`)
hasn't been exercised by a real test yet — same next test should also
capture this.

- [ ] **Real test with the PC on**: confirm `wol-trigger-skipped-host-active`
      now correctly fires (instead of `wol-trigger-fired`) within ~2s of a
      controller reconnect while the PC is on, and the lightbar does **not**
      pulse.
- [ ] **Real PC-off retest**: confirm WOL still fires within the bounded
      window (not indefinitely delayed) when the PC is genuinely off — the
      fifteenth-bug-fixed end-to-end path must stay unaffected.
- [ ] Same test(s): check for `board-transport-recovery-reboot`/
      `conn-disconnect-retry-sent` (still unexplained) and confirm/deny
      whether it recurs.
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
