# Task

Current + next task only. Completed work is purged from here into
`CHANGELOG.md` as it finishes — this file should stay short. Architecture
knowledge and known issues belong in `DECISIONS.md`, not here.

## Current task

The `ObserveHost` window (previous round) still didn't skip WOL in a real
test where it should have (companion app open, USB genuinely mounted/
active, PC confirmed on) — `wol-trigger-fired` fired instead of
`wol-trigger-skipped-host-active`. Rather than keep guessing at the
state-machine logic, added deep diagnostics: `usb_host_active_debug_bits()`
(bitmask of every flag `usb_host_active()` depends on) plus three new
trace stages (`ObserveHostBegin`, `ObserveHostSampleEdge`,
`ObserveHostWindowElapsed`) so the next trace shows exactly what the
window observed instead of requiring inference. See `CHANGELOG.md` for the
full writeup. Firmware/companion rebuilt (`win-unpacked` refreshed).

Still open from three rounds ago: the board-transport-recovery-reboot
instrumentation (`BoardTransportRecoveryReboot`/`ConnDisconnectRetrySent`)
hasn't been exercised by a real test yet — same next test should capture
this too.

- [ ] **Real test with the PC on, companion app open** (reproduce the same
      conditions as the failing test). Read `ds5bridge-wol-debug.log` for:
      - `observe-host-begin`'s `detail` bitmask — what did
        `usb_host_active_debug_bits()` read the instant the window armed?
        (bit0=usb_mounted, bit1=tud_inited, bit2=tud_suspended,
        bit3=usb_host_suspended-flag, bit4=controller_transport_ready,
        bit5=controller_transport_attached)
      - every `observe-host-sample-edge` during the window — did
        `usb_host_active()` ever read true at all? If yes, for how long
        before flipping back (compare against the 100ms sustain
        threshold)? If never, which bit(s) stayed wrong the whole time?
      - `observe-host-window-elapsed`'s final bitmask, if the window did
        time out.
- [ ] Same test: check for `board-transport-recovery-reboot`/
      `conn-disconnect-retry-sent` (still unexplained) and confirm/deny
      whether it recurs.
- [ ] Once the real cause is visible: fix `usb_host_active()`/the
      `ObserveHost` logic accordingly — no more guessing until this data
      is in hand.
- [ ] **Real PC-off retest**: confirm WOL still fires within the bounded
      window when the PC is genuinely off — the fifteenth-bug-fixed
      end-to-end path must stay unaffected by all of this.
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
