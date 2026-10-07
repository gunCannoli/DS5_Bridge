# Task

Current + next task only. Completed work is purged from here into
`CHANGELOG.md` as it finishes — this file should stay short. Architecture
knowledge and known issues belong in `DECISIONS.md`, not here.

> Upstream sync: fork is current on **v1.7.1** as of 2026-09-06 (three
> "keep both" conflicts, no `COMMAND_ID` renumbering) — see `CHANGELOG.md`.
> DS5_Bridge PR 120 (Wake-on-LAN) still open upstream against `port-dev`.

## Current state

Four fork features, all documented in `CUSTOM.md`:

| # | Feature | State |
|---|---|---|
| 1 | Wake-on-LAN over Wi-Fi (firmware + companion) | shipped in DS5_Bridge PR 120, open upstream |
| 2 | Auto Switch Audio on Jack (companion only) | **DS5_Bridge PR 147 closed 2026-10-07** to give the feature more real-world testing (see below). Branch `pr/auto-switch-audio-on-jack` kept, but it predates the 2026-10-07 fix. |
| 3 | Audio-aware Idle Disconnect (firmware) | **DS5_Bridge PR 146 open** (branch `pr/idle-disconnect-audio-aware` off upstream/main). Hardware-tested. Awaiting maintainer review. |
| 4 | Speaker-audio stutter | investigated and **reverted** — see `PLAN-audio-stutter.md`; nothing shipped |

Repo hygiene work completed 2026-09-07:

- **Cross-repo pollution removed.** This fork had been writing
  `owner/repo#N`-style refs in commit messages and code comments, which
  GitHub turns into backlink comments on other projects' trackers. Sources,
  docs, and **all 86 commit messages** are now clean (`tools/scrub-history.sh`,
  already run; backup refs under `refs/backup/`). Verified: 0 polluting refs.
- **`agents/` folder** — all fork tracking docs moved here; the repo root
  keeps only `AGENTS.md` plus upstream's `README.md`/`CONTRIBUTING.md`.
- **`CONTRIBUTING-FORK.md`** — the durable rule for both the pollution ban
  and the PR-vs-local-patch policy.

## ACTIVE: Auto Switch Audio unplug fix — needs hardware smoke test

Fixed on `feature/wol-wifi` (`ff08d58`, `4c2ba3d`). The fix and the
root cause (stale RDP-era fallback cache, stale `SESSIONNAME`, per-poll retry
storm) are in CHANGELOG 2026-10-07. Packaged companion rebuilt; zero
AudioHelper crash events since.

- [ ] Smoke test with the controller: plug/unplug repeatedly → default follows
      (TV on unplug, never the controller speaker); power the controller off
      with the headset in → default goes to the TV; RDP in and out, then
      unplug → still goes to the TV. Expect no 1026 events in Event Viewer.
- [ ] Live with it for a while (RDP sessions, TV off/on, controller sleeps).
- [ ] When it's ready to go upstream again: fold `ff08d58` + `4c2ba3d` into
      `pr/auto-switch-audio-on-jack` (still one clean commit off upstream/main,
      amend + **force-push**, needs confirmation), then reopen PR 147 or open a
      new one.

## ACTIVE: WOL boot-sequence investigation — done for now

**Reported (post-v1.7.1 + `7a7eaa8` keep-wake-receiver-online USB rework):**
controller disconnects as the PC starts booting after a WOL wake, and the
green lightbar pulse stopped appearing.

**Outcome:** built a full board-trace diagnostic branch
(`debug/wol-boot-trace`, 8 commits off `feature/wol-wifi` — see
`DECISIONS.md`) and captured 3 real runs (`final` x2 + `smoke` x1). **The
regression did not reproduce** — controller survived every boot, pulse
worked, `ObserveHost` correctly saw `suspended|host-suspended` on the
bridge-only topology and fired WOL, the descriptor-topology teardown
happened with the controller connected and it survived. Likely the report
predates a build that already resolved it.

**What the trace *did* show, and the fix shipped:**
- Every cold WOL boot, Wi-Fi connect attempt #1 returned
  `CYW43_LINK_BADAUTH` (join_state 0x4) with a correct passphrase, then
  attempts #2/#3 over ~35s before a link. Fixed on `feature/wol-wifi`:
  `fix(wolwifi): fast leave-and-retry on a first Wi-Fi BADAUTH` — explicit
  `cyw43_wifi_leave()` + 1.5s backoff (was 10s) for the first BADAUTH
  retry only; genuinely-wrong password still capped at 2 attempts.
  Firmware only, no protocol change. Builds `final`, host tests pass.
- User is also re-entering the WOL Wi-Fi credentials in the companion.
- The un-suppressed `usb-controller-power-off` at trace seq=8 was a
  **no-op** (`hid_control_cid == 0`, no controller connected) — not a bug.
  `wolwifi_wake_in_progress()` already covers the real windows
  (`g_send_pending` persists across the connect/retry sequence).

**If the drop recurs:** flash `debug/wol-boot-trace`'s `final` build, repro
(ideally: controller already awake in hand, PC asleep, press PS — the
timing most likely to expose an un-suppressed power-off), read
`<Electron logs>/ds5bridge-wol-debug.log`. Otherwise the debug branch can
be retired (don't merge it).

## NEXT: audio-output stutter debug branch

User reports audio hiccups on playback to the controller even at haptics
buffer 120 (≈40 ms). New branch `debug/audio-output-trace` off
`feature/wol-wifi` (NOT stacked on `debug/wol-boot-trace`). Full plan:

- **Build:** new `audiodebug` variant in `tools/build-firmware.ps1` =
  `final` + `-DDS5_DIAGNOSTICS_PRESET=audio` (existing 96-slot
  `DS5_AUDIO_DEBUG_ENABLED` ring + `audio_debug_stats`; NO UART logs, NO
  trigger/feedback traces). Uses the SRAM budget for audio, not WOL.
  Confirm it links (audio ring alone ~1.3 KB; `final` had headroom).
- **Firmware (all behind `DS5_AUDIO_DEBUG_ENABLED`, zero cost otherwise):**
  the existing ring is thin on the BT *send* cadence, which is where a
  40 ms-buffer hiccup lands. Add:
  - `AudioDebugBtSendGap` at the `bt_write_audio_stream()` call in
    `try_send_pending_audio_batch()`: Δt since last successful send,
    return value, `speaker_opus_fifo` + `audio_fifo` levels,
    `pending_audio_haptics_count`.
  - `AudioDebugBtSendBlocked` when the batch can't assemble
    (`speaker_opus_batch_ready()` false / `pending < AUDIO_BATCH_FRAMES`)
    — opus-starved vs haptics-starved vs BT backpressure.
  - Ring-log the generation-drop (stats-only today) with stale-vs-current
    generation, so a mid-stream persona/route-toggle flush is visible.
  - Emit `AudioDebugCpuLoad` (23) each drain with
    `audio_loop_runtime_max_us` / `audio_loop_gap_max_us`.
  - Add a periodic (~250 ms) unconditional entry to
    `audio_debug_packet_log_budget` so a hiccup 10 s into playback is
    still captured, not just the first 4 packets after stream start.
- **Companion:** dedicated `<logs>/ds5bridge-audio-debug.log` file drain
  (today audio-debug lines only go to the in-memory Diagnostics-tab ring,
  capped 300, lost on disconnect). Same `wolDebugLogDirectory`-style
  plumbing. Per-poll `stats` timeline line + one line per ring event,
  fields decoded.
- Env: `DS5_BRIDGE_AUDIO_DEBUG_DIAGNOSTICS=1` (or `DS5_BRIDGE_DIAGNOSTICS=audio`).
- Commits `diag(audio): ...`; DECISIONS.md entry mirroring the WOL one.

## Other next tasks (unchanged)

- [ ] **Scrub `origin/feature/wol-wifi`** (the clean PR 120 branch) the same
      way `wol-wifi-full-history` was scrubbed, before PR 120 is next updated.
- [ ] Respond to review on PR 146. If revision is needed the branch is
      `pr/idle-disconnect-audio-aware`, one clean commit off `upstream/main`
      -- amend + force-push, do not merge fork-branch history in.
- Local-only items (full-window settings modal, the `:disabled` flicker fix)
  stay on `feature/wol-wifi` only and are NOT in either PR.

## Known, deliberately not fixed

- **`debug` build variant does not link** — fails `verify_core1_sram`'s
  runtime-heap minimum. Pre-existing (~85.6 KB free vs 87312 required);
  `debug/wol-boot-trace`'s rings make it tighter (~84 KB). Use `final` for
  smoke tests — the board trace is HID-transported and does not need the
  UART `debug` variant anyway.
- **Waveshare top-of-flash BTstack bank.** DS5Dongle PR 241 reports that
  accesses near the end of the advertised 16 MiB stall on this board
  (config-load hang → no BT pairing, or `VID_0000&PID_0002`). DS5_Bridge uses
  the SDK default bank, which sits in exactly that region. We have **not**
  reproduced it across many real-hardware cycles. If it ever appears, the fix
  is to relocate low: `-DPICO_FLASH_BANK_STORAGE_OFFSET=0x003FD000`.
