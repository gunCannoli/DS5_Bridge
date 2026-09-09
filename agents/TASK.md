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

## ACTIVE: audio-output hiccup — `debug/audio-output-trace` branch built

User reports audible hiccups on audio played to the controller even at
haptics buffer 120 (≈40 ms). Diagnostic branch off `feature/wol-wifi`
(NOT stacked on `debug/wol-boot-trace`). **3 commits, built + tested,
ready to flash** — see `DECISIONS.md` for the full writeup:

1. `diag(build): add audiodebug firmware variant` — `final` +
   `-DDS5_DIAGNOSTICS_PRESET=audio`. Links clean.
2. `diag(audio): trace the BT send-batch assembly path` — new ring events
   `AudioDebugBatchBlocked` (24), `AudioDebugBatchSent` (25),
   `AudioDebugGenerationFlush` (26), all behind `DS5_AUDIO_DEBUG_ENABLED`
   so `final`/`smoke` are byte-identical.
3. `diag(audio): dedicated ds5bridge-audio-debug.log file drain` — no
   protocol bump; companion writes every audio-debug line + the per-poll
   `[AudioStats]` line to a file.

### NEXT: capture an audio hiccup trace
- `git checkout debug/audio-output-trace`
- `$env:PICO_SDK_PATH="C:\auto\arduino\build\pico-sdk"; .\tools\build-firmware.ps1 audiodebug -WithCompanion`
- Flash `firmware/ds5-bridge-1.71-wol-audiodebug.uf2`.
- Companion needs `DS5_BRIDGE_AUDIO_DEBUG_DIAGNOSTICS=1` (or
  `DS5_BRIDGE_DIAGNOSTICS=audio`) in its environment — `rebuild-companion.ps1`
  does NOT set this, so either export it before launching the packaged app,
  or run `npm run dev` with it set.
- Play audio through the controller (speaker + headset in the jack, a
  movie or music), reproduce the hiccup, then read
  `<Electron logs>/ds5bridge-audio-debug.log` (on Windows
  `%APPDATA%\DS5 Bridge\logs\`).
- Reading it: `[BatchSent] ... HICCUP` = a >30 ms gap on the audio.cpp
  send side; `[BatchBlocked] reason=opus-not-ready` = opus encoder on
  core 1 behind; `[GenFlush]` = pipeline flushed (persona/route toggle);
  `[BtEvent]` late-audio = BT transport itself late. `[AudioStats]` line
  each poll has the running max/count timeline.

### THEN: fix on `feature/wol-wifi`, then retire this branch
Depends on what dominates the trace. Once fixed, revert the 3 commits
(or don't merge).

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
