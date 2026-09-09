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
| 2 | Auto Switch Audio on Jack (companion only) | **DS5_Bridge PR 147 open** (branch `pr/auto-switch-audio-on-jack` off upstream/main). Hardware-tested, incl. the multi-output dropdown. Awaiting maintainer review. |
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

## ACTIVE: WOL boot-sequence regression — diagnose, then fix

**Symptom (post-v1.7.1 merge + `7a7eaa8` keep-wake-receiver-online USB
rework):** PC off → PS button sends WOL → PC starts booting → controller
disconnects as soon as the PC powers on / starts the boot sequence. Also:
the green lightbar pulse that used to show "waiting for the PC to confirm
boot" no longer appears when the magic packet is sent.

**Leading hypotheses** (see `DECISIONS.md` "Debug branch:
`debug/wol-boot-trace`" for the full writeup):
1. `ObserveHost` reads a false "host active" from the new bridge-only USB
   descriptor topology being enumerated against a still-booting PC → WOL
   trigger silently aborted (→ no pulse; and no
   `wolwifi_wake_in_progress()` → USB-suspend controller power-off no
   longer suppressed → controller drop). Single mechanism, explains both
   symptoms.
2. `usb-topology-reconnect-begin`'s full endpoint teardown
   (`tud_disconnect` + `dcd_edpt_close_all` + `DCD_EVENT_UNPLUGGED`)
   firing mid-boot while the BT session is fresh / Wi-Fi is contending
   (HCI `0x22` radio-contention signature).
3. `usb-suspend-armed armed_from=tud_umount_cb` + the poll check widening
   to `(usb_host_suspended || !usb_mounted)` → `bt_power_off_controller()`
   during the PC's boot re-enumeration churn.

### Step 1 — DONE: `debug/wol-boot-trace` branch built
6-commit diagnostic patch off `feature/wol-wifi` (restores the
`a09323b`-stripped board trace ring + extends it for the new USB topology
state machine + a periodic full-state snapshot ring; companion drains both
to `<app logs>/ds5bridge-wol-debug.log` with every field decoded). Builds
clean on `final`/`smoke`; `debug` overflows SRAM (accepted — the ring is
HID-transported, doesn't need the UART variant). Full commit list + revert
instructions in `DECISIONS.md`.

### Step 2 — NEXT: capture a real trace
- `git checkout debug/wol-boot-trace`
- `$env:PICO_SDK_PATH="C:\auto\arduino\build\pico-sdk"; .\tools\build-firmware.ps1 final -WithCompanion`
- Flash `firmware/ds5-bridge-1.71-wol-final.uf2`, reconfigure WOL once if
  needed (flash may be empty), then: PC off → PS button → let it boot.
- Read `ds5bridge-wol-debug.log` (Electron `app.getPath('logs')`, on
  Windows `%APPDATA%\ds5-bridge-companion\logs`). Also worth one run on the
  `smoke` variant (`WOL_ALWAYS=ON`, bypasses `ObserveHost`) — if the pulse
  returns and the controller survives there, hypothesis 1 is confirmed.

### Step 3 — fix, on `feature/wol-wifi` (not the debug branch)
Depends on what the trace shows. Likely candidates:
- **If hypothesis 1:** `usb_host_active()` / a new
  `usb_full_persona_mounted()` must exclude the bridge-only topology
  (`usb_mounted && !usb_attached_bridge_only`); point `drive_observe_host()`
  at that. Or ship this board with `WOL_ALWAYS=ON` (documented escape
  hatch — deployment always has the Pico in the PC being woken).
- **If hypothesis 2/3:** gate the topology-swap `tud_disconnect()` /
  `arm_suspend_disconnect()` behind `!wolwifi_wake_in_progress()`, and/or
  widen `wolwifi_wake_in_progress()` coverage.
Keep the fix minimal + rebase-friendly per `AGENTS.md`. Then revert the
`debug/wol-boot-trace` commits (or just don't merge that branch).

### Step 4 — THEN PIVOT: audio-output hiccup debug branch
**Not before Step 3 lands.** Separate branch `debug/audio-output-trace`
off `feature/wol-wifi` (NOT stacked on `debug/wol-boot-trace` — that one
gets reverted). Purpose: user reports audio hiccups on playback to the
controller even at haptics buffer 120 (≈40 ms). Plan:

- **Build:** new `audiodebug` variant in `tools/build-firmware.ps1` =
  `final` + `-DDS5_DIAGNOSTICS_PRESET=audio` (existing 96-slot
  `DS5_AUDIO_DEBUG_ENABLED` ring + `audio_debug_stats`; NO UART logs, NO
  trigger/feedback traces — keep it lean). This branch uses the SRAM
  budget for audio, not WOL — so it does NOT carry the WOL trace rings.
  Confirm `audiodebug` links (audio ring alone is ~1.3 KB, `final` had
  headroom).
- **Firmware (all behind `DS5_AUDIO_DEBUG_ENABLED`, zero cost otherwise):**
  new trace points on the BT *send cadence* — the existing ring is thin
  there and that's where a 40 ms-buffer hiccup lands:
  - `AudioDebugBtSendGap` at the `bt_write_audio_stream()` call in
    `try_send_pending_audio_batch()`: Δt since last successful send,
    return value, `speaker_opus_fifo` + `audio_fifo` levels,
    `pending_audio_haptics_count`.
  - `AudioDebugBtSendBlocked` when the batch can't assemble
    (`speaker_opus_batch_ready()` false / `pending < AUDIO_BATCH_FRAMES`)
    — distinguishes opus-starved vs haptics-starved vs BT backpressure.
  - Ring-log the generation-drop (currently stats-only) with stale-vs-
    current generation, so a mid-stream persona/route-toggle flush is a
    visible event.
  - Emit `AudioDebugCpuLoad` (code 23) each drain with
    `audio_loop_runtime_max_us` / `audio_loop_gap_max_us` so core-1 loop
    stalls correlate.
  - Add a periodic (~250 ms) unconditional entry to
    `audio_debug_packet_log_budget` so a hiccup 10 s into playback is
    still captured, not just the first 4 packets after stream start.
- **Companion:** dedicated `<logs>/ds5bridge-audio-debug.log` file drain
  (today the audio-debug lines only go to the in-memory Diagnostics-tab
  ring, capped 300, lost on disconnect). Same `wolDebugLogDirectory`-style
  plumbing. Decode `audio_debug_stats` deltas per poll as a running
  `stats` timeline line, plus one line per ring event.
- Env: `DS5_BRIDGE_AUDIO_DEBUG_DIAGNOSTICS=1` (or `DS5_BRIDGE_DIAGNOSTICS=audio`).
- Commit series `diag(audio): ...`; DECISIONS.md entry mirroring the WOL
  one (what it covers, how to drop it).

## Other next tasks (unchanged)

- [ ] **Scrub `origin/feature/wol-wifi`** (the clean PR 120 branch) the same
      way `wol-wifi-full-history` was scrubbed, before PR 120 is next updated.
- [ ] Respond to review on PRs 146 / 147. If revision is needed the branches
      are `pr/idle-disconnect-audio-aware` and `pr/auto-switch-audio-on-jack`,
      each one clean commit off `upstream/main` -- amend + force-push, do not
      merge fork-branch history in.
- Local-only items (full-window settings modal, the `:disabled` flicker fix)
  stay on `feature/wol-wifi` only and are NOT in either PR.

## Known, deliberately not fixed

- **`debug` build variant does not link** — fails `verify_core1_sram`'s
  runtime-heap minimum. Pre-existing (~85.6 KB free vs 87312 required),
  unrelated to any fork feature; the `debug/wol-boot-trace` rings make it
  tighter still (~84 KB). Use `final` for smoke tests — the board trace is
  HID-transported and does not need the UART `debug` variant anyway.
- **Waveshare top-of-flash BTstack bank.** DS5Dongle PR 241 reports that
  accesses near the end of the advertised 16 MiB stall on this board
  (config-load hang → no BT pairing, or `VID_0000&PID_0002`). DS5_Bridge uses
  the SDK default bank, which sits in exactly that region. We have **not**
  reproduced it across many real-hardware cycles. If it ever appears, the fix
  is to relocate low: `-DPICO_FLASH_BANK_STORAGE_OFFSET=0x003FD000`.
