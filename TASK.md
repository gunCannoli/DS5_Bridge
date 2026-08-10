# Task

Current + next task only. Completed work is purged from here into
`CHANGELOG.md` as it finishes — this file should stay short. Architecture
knowledge and known issues belong in `DECISIONS.md`, not here.

## Current task

Fourteenth bug: WOL fires ~61 real seconds after the controller-connect
trigger (see `DECISIONS.md`'s CYW43 radio-contention entry and
`CHANGELOG.md`'s fourteenth-bug entry for background). Root cause not yet
found — trace instrumentation was added (`WolConnectStarted`,
`WolWifiLinkLostAfterConnect`, `WolWifiConnected`, `WolWifiBackoffElapsed`)
to make it visible on the next real test.

- [ ] **Real PC-off wake test with the new instrumentation**, companion app
      left open/polling continuously through the whole test. WOL config
      already persisted in flash. Read `ds5bridge-wol-debug.log`:
      - Every `wol-connect-started` line between `wol-trigger-fired` and
        the eventual `wol-resend-begin`/`wol-resend-gave-up` — how many
        connect attempts happened, at what board-time each started.
      - For each attempt: does it end in `wol-wifi-connected` (detail says
        whether a resend cycle actually started),
        `wol-wifi-link-lost-after-connect`, `wol-wifi-assoc-timeout`,
        `wol-dhcp-wait-timeout`, or nothing (still unexplained)?
      - Does `wol-wifi-backoff-elapsed` appear between attempts, and does
        the timing now account for the full delay?
      - `wol-wifi-connected detail=0` (connected but `g_send_pending` was
        already false) is the specific "silently connected, did nothing"
        case to confirm or rule out.
- [ ] Also re-verify: no HCI `0x22` disconnect (thirteenth-bug fix should
      still hold), no unexpected `wol-trigger-debounced`/
      `wol-connect-retries-exhausted` entries (debounce/retry-cap guards
      firing when they shouldn't).
- [ ] Once the trace explains the gap, design and implement the actual fix.

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
