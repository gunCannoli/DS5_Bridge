# Task

Current + next task only. Completed work is purged from here into
`CHANGELOG.md` as it finishes — this file should stay short. Architecture
knowledge and known issues belong in `DECISIONS.md`, not here.

## Current task

**`ObserveHost` confirmed working on real hardware.** A real test (PC on,
companion app open) traced cleanly: window armed at `Ready`
(`observe-host-begin` detail=2, USB not yet mounted for this fresh
session), controller-type handshake completed in 22ms
(`conn-controller-type-identified`, matching the earlier 23ms
measurement), `usb_host_active()` flipped true ~217ms after `Ready`
(`observe-host-sample-edge` detail=51 — mounted, not suspended, transport
ready+attached), and exactly 100ms later (matching
`WOL_OBSERVE_HOST_SUSTAIN_MS`) the window correctly concluded sustained-
active and fired `wol-trigger-skipped-host-active` — no Wi-Fi connect
attempt, no lightbar WOL pulse. Whole detect-and-skip cycle took ~340ms
end to end, well inside the 2s window. See `CHANGELOG.md` for the full
writeup.

Still open: the board-transport-recovery-reboot instrumentation
(`BoardTransportRecoveryReboot`/`ConnDisconnectRetrySent`) hasn't fired in
any test yet (neither confirming nor ruling out that it still happens) —
not urgent, doesn't block the WOL feature itself.

- [ ] **Real PC-off retest**: confirm WOL still fires within the bounded
      window (not suppressed) when the PC is genuinely off — the
      fifteenth-bug-fixed end-to-end path, combined with the now-working
      host-alive gate, should show the full correct behavior: skip when on,
      fire promptly when off.
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
