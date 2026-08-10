# Task

Current + next task only. Completed work is purged from here into
`CHANGELOG.md` as it finishes — this file should stay short. Architecture
knowledge and known issues belong in `DECISIONS.md`, not here.

## Current task

Just added a host-alive gate (skip WOL entirely if `usb_host_active()`
shows the target PC already on) per user request, after noticing the
lightbar pulse firing unnecessarily. See `DECISIONS.md` for the design.
Firmware builds cleanly (default gate-on, `WOL_ALWAYS` escape hatch, and
the non-WOL default board target); no companion-app changes needed.

- [ ] **Bench-test the host-alive gate**: with the target PC on (companion
      app running against it), connect the controller and confirm
      `wol-trigger-skipped-host-active` appears in the board trace instead
      of `wol-trigger-fired`/`wol-resend-begin` — and confirm the lightbar
      does **not** pulse (the actual symptom being fixed).
- [ ] **Real PC-off retest**: confirm WOL still fires normally when the PC
      is genuinely off — the fifteenth-bug-fixed path (and the
      end-to-end success already confirmed on 2026-08-10) must be
      unaffected by this new gate.
- [ ] Flash and confirm `-DWOL_ALWAYS=ON` actually bypasses the gate if a
      board/BIOS combination needs it (not urgent — only matters if a real
      board hits the known limitation described in `DECISIONS.md`).

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
