# Task

Current + next task only. Completed work is purged from here into
`CHANGELOG.md` as it finishes — this file should stay short. Architecture
knowledge and known issues belong in `DECISIONS.md`, not here.

## Current task

Fifteenth bug just fixed (not yet verified): the fourteenth bug's new trace
instrumentation showed a ~27s gap between `wol-trigger-fired` and the first
`wol-connect-started` on a fresh boot with persisted config — root cause
found (`wolwifi_init()` left the connect state machine stuck in
`Unconfigured` even after loading a valid SSID from flash; only the
SSID/password setters ever call `enter_state(Idle)`, so the state machine
only escaped once the companion app's slow settings-reapply happened to
re-send the SSID). See `DECISIONS.md`'s flash-persistence entry for the
full writeup. Fixed: `wolwifi_init()` now enters `Idle` directly when a
valid SSID was loaded. Debug firmware and companion app (`win-unpacked`)
both rebuilt.

- [ ] **Real PC-off wake test with this fix.** WOL config already persisted
      in flash. Confirm: (a) `wol-connect-started` now appears within
      ~1 tick of `wol-trigger-fired` (no multi-second gap), (b) WOL actually
      reaches the PC before it's manually powered on, (c) no HCI `0x22`
      disconnect (thirteenth-bug fix should still hold), (d) no unexpected
      `wol-trigger-debounced`/`wol-connect-retries-exhausted` entries.
- [ ] If a gap still remains, the fourteenth-bug trace stages
      (`wol-wifi-connected`, `wol-wifi-link-lost-after-connect`,
      `wol-wifi-backoff-elapsed`) are still in place to diagnose further.

## Next task

- If the real wake test fully succeeds (WOL fires promptly, PC wakes, no
  disconnect): proceed to Phase 9 (failure-behavior verification) and
  Phase 10 (PR prep) — see `README.md`/git history for what those phases
  cover, or ask if the original plan document is needed.
- If it fails differently: the debug Ping/WOL Test tooling (Wi-Fi already
  working) should narrow it down quickly. Likely candidates not yet ruled
  out: the magic packet not reaching the target NIC specifically (the
  debug ARP ping only proves general network reachability, not that the
  NIC's WOL listener is armed), or an OS/BIOS-level WOL setting on the
  target PC (Windows fast-startup/hybrid-shutdown can silently disable WOL
  after a "shutdown" that isn't a true power-off).
