# AGENTS.md

Working agreement for AI-assisted work on this fork (`gunCannoli/DS5_Bridge`).

## Purpose of this fork

This fork exists to develop **Wake-on-LAN over Wi-Fi** as an optional, isolated
feature on top of upstream `SundayMoments/DS5_Bridge`, targeting the
`port-dev` branch, with the end goal of submitting it upstream as a PR.

Treat this fork's changes as **our own patch**, not a divergent product:

- Keep the WOL feature additive and isolated (new files + minimal, well-marked
  hook points in existing files) so it stays easy to rebase/cherry-pick onto
  future upstream `port-dev` commits.
- Prefer new files (`wolwifi.h`, `wolwifi.cpp`) over modifying existing logic.
- Where existing files must change, keep the diff minimal and localized (a
  single call site, a new case in a switch, a new field in a settings object)
  rather than restructuring.
- Do not refactor or "clean up" unrelated upstream code. Any such change makes
  future rebases harder and is out of scope.
- Follow existing project conventions (naming, error handling, logging style,
  companion app patterns) rather than introducing new ones.

## Tracking files (repo root)

All-caps filenames (`TASK.md`, `CHANGELOG.md`, `DECISIONS.md`,
`AGENTS.md`) — keep them that way; this filesystem is case-insensitive so a
lowercase variant can silently coexist, but git tracks the all-caps name.

- **TASK.md** — **current + next task only, kept lean.** As soon as a task
  is done, its outcome is purged from here: a one-line pointer stays if
  useful ("see CHANGELOG.md's Nth-bug entry"), but the narrative goes to
  CHANGELOG.md, not this file. This file should be short enough to read in
  full every session, not an accumulating log — if it's growing long,
  something that finished wasn't purged yet.
- **CHANGELOG.md** — durable record of completed work, newest first. This
  is where bug-fix narratives, phase completions, and "what happened and
  when" belong once done. Fine for this to be long; it's a history, not
  something read in full every session.
- **DECISIONS.md** — **only** genuine architecture decisions and
  known-issue/root-cause discoveries that should inform *future* work —
  not a log of every bug fixed (that's CHANGELOG.md). The test: would a
  future agent hitting a similar symptom, or a PR reviewer asking "why does
  this code look like this," need to know this? If it's just "here's what
  we tried and what happened," it's a changelog entry. If it's "here's a
  recurring failure mode / wire-format choice / API gotcha that will matter
  again," it's a decision. Keep this short enough to actually serve as a
  knowledge base — bloat defeats the purpose.
- **AGENTS.md** — this file.

## SDK / library install location

All SDKs and libraries needed for this work install to **`C:\auto\arduino\build`**,
not project-local or user-profile locations.

- Pico SDK is already present at `C:\auto\arduino\build\pico-sdk` — use this
  checkout (`PICO_SDK_PATH=C:\auto\arduino\build\pico-sdk`) rather than
  cloning a new one.
- Any additional toolchain/SDK/library needed for the WOL feature (ARM
  toolchain, Ninja, lwIP if it ships separately from the Pico SDK checkout,
  etc.) installs under `C:\auto\arduino\build\<name>` following the same
  convention, so everything stays in one predictable place across sessions.
- Before installing anything new, check `C:\auto\arduino\build` first — it
  may already be there.

## Push / GitHub sync guidelines

Keep `origin` (the fork) reasonably up to date as work progresses, but don't
push on every single commit.

- Push when a phase (or a meaningful chunk of one) from TASK.md is complete
  and builds/tests cleanly — e.g. after Wi-Fi bring-up compiles, after the
  companion app changes are wired end-to-end, after a smoke-test round passes.
- Push before ending a work session, so nothing is stranded only in the local
  worktree.
- Prefer pushing a branch (`feature/wol-wifi`) over pushing to a shared
  branch; never force-push without explicit confirmation.
- Commit locally frequently (small, logically separated commits per
  DECISIONS.md/TASK.md phase breakdown) — pushing is about syncing to GitHub,
  not about commit granularity.

## Build output locations (use upstream's existing conventions, don't invent new ones)

Both the board firmware and the companion app already have defined output
locations upstream. Always use these — don't add a parallel `dist/` or similar,
since that adds nothing but rebase friction for zero benefit.

- **Board firmware UF2**: `build/waveshare/ds5-bridge.uf2`, produced by
  `boards/build_waveshare_rp2350b_plus_w.sh` (default board target) or
  `build/<name>/ds5-bridge.uf2` for other `-B <dir>` CMake configure targets
  (e.g. our manual test builds used `build/waveshare_test`). This is CMake's
  own build directory (gitignored via `build`/`build-*` in `.gitignore`), not
  a separate release/dist step — upstream does not currently stage a "final"
  firmware release copy anywhere else.
- **Companion app installer**: `companion/artifacts/installer/` (NSIS `.exe`),
  produced by `npm run installer:win` inside `companion/` via electron-builder
  (`companion/package.json`'s `build.directories.output`).
- **Companion app portable/debug package**: `companion/artifacts/DS5 Bridge-win32-x64-<timestamp>/`,
  produced by `npm run package:win` (`companion/scripts/package-win.mjs`) —
  a timestamped folder tree, not a single exe, used for quick manual testing
  without building a full installer.
- **Companion app local unpacked build**: `C:\game\DS5 Bridge App`, produced
  by `npm run package:win:local` — same unpacked build as `package:win` but
  written to this fixed path instead of a timestamped `artifacts/` folder, so
  it can be pointed at directly (e.g. a Task Scheduler action running
  `"C:\game\DS5 Bridge App\DS5 Bridge.exe" --start-in-tray`). The script wipes
  this directory before each rebuild. Outside the repo tree, so nothing to
  gitignore.
- Both `companion/artifacts` and `build`/`build-*` are already gitignored at
  the repo root; nothing extra was needed there.

If a future version of this feature needs a genuinely new output location
(e.g. bundling the Waveshare UF2 into the companion installer the way
`pico-universal-flash-nuke.uf2` is bundled via `companion/firmware/` +
`extraResources` in `package.json`), record that decision in `DECISIONS.md`
with the reasoning, so it's clear it was an intentional addition and not
drift from upstream's layout.

## Rebuild rules — what to rebuild after a change

The two halves of this project build independently and don't need to be
rebuilt together. Only rebuild what you actually touched.

### Firmware-only changes (anything under `src/`, `boards/`, root `CMakeLists.txt`)

Rebuild the board firmware. The companion app does NOT need rebuilding —
it talks to whatever firmware is currently flashed over the existing HID
protocol, it isn't compiled against firmware source.

```bash
export PICO_SDK_PATH="C:/auto/arduino/build/pico-sdk"
./boards/build_waveshare_rp2350b_plus_w.sh
# UF2 at build/waveshare/ds5-bridge.uf2
```

For a quick one-off/manual configure (e.g. testing a CMake option change in
isolation) instead of the convenience script:

```bash
cmake -S . -B build/waveshare -G Ninja \
  -DWAVESHARE_RP2350B_PLUS_W_BUILD=ON -DENABLE_COMPANION=ON \
  -DPICO_NO_COPRO_DIS=1 -DPICO_SDK_PATH="C:/auto/arduino/build/pico-sdk"
cmake --build build/waveshare --target ds5-bridge
```

`-DENABLE_COMPANION=ON` is required for a real build (it's what compiles
`companion.cpp`, including all the WOL command handlers) — the CMake default
is off. See "Local build environment notes" below if the final link/UF2 step
crashes; the actual compile+link already succeeded if you see "Verified
complete live firmware hot paths..." before the crash.

To also sanity-check the default (non-Waveshare) board still builds after a
change to shared firmware code (anything outside `wolwifi.h/.cpp`,
`boards/headers/lwipopts.h`):

```bash
cmake -S . -B build/default -G Ninja -DENABLE_COMPANION=ON \
  -DPICO_NO_COPRO_DIS=1 -DPICO_SDK_PATH="C:/auto/arduino/build/pico-sdk"
cmake --build build/default --target ds5-bridge
```

Flashing: hold BOOTSEL, plug in, drag the UF2 onto the mounted drive — or
use the companion app's Firmware > Mount + Flash buttons once it's running
against the currently-flashed firmware.

### Companion-app-only changes (anything under `companion/`)

Rebuild the companion app. The firmware does NOT need rebuilding.

```bash
cd companion
npm run typecheck   # fast correctness check, run this first
npx vitest run src  # full test suite
npm run dev          # build + launch electron for interactive testing
```

To produce a distributable build:

```bash
npm run installer:win   # NSIS installer -> companion/artifacts/installer/
# or, for a quick portable folder without building a full installer:
npm run package:win     # -> companion/artifacts/DS5 Bridge-win32-x64-<timestamp>/
# or, to rebuild the fixed local copy used by the tray auto-start task:
npm run package:win:local   # -> C:\game\DS5 Bridge App
```

The desktop shortcut points at `companion/artifacts/installer/win-unpacked/DS5 Bridge.exe`
directly (not a separate installed copy), so `npm run installer:win` is what
actually needs to run to make the shortcut launch fresh code -- rebuilding
only `companion/artifacts/DS5 Bridge-win32-x64-<timestamp>/` via
`package:win` does NOT update what the shortcut launches. Likewise, if
`C:\game\DS5 Bridge App` is what a scheduled task/shortcut launches, that
copy only gets updated by explicitly running `npm run package:win:local`.

If the app is currently running, close it first (it locks `DS5 Bridge.exe`
and the rebuild will fail) -- always OK to close and rebuild without asking
first, per explicit standing permission:

```powershell
Get-Process | Where-Object { $_.Path -like '*win-unpacked*' } | Stop-Process -Force -Confirm:$false
```

### Changes touching both (e.g. a new companion protocol command, like WOL's
### SET_WOL_* commands and their `src/companion.cpp` handlers)

Rebuild both, in either order, but test them together before considering the
change done — a protocol change is only correct if both sides agree on wire
format (`COMMAND_ID` values, payload layout, `PROTOCOL_MAJOR`/`MINOR`).

1. Rebuild firmware (above), flash it to the board.
2. Rebuild/run the companion app (above) against that flashed firmware.
3. Exercise the actual feature through the UI, not just typecheck/build
   success — protocol mismatches don't show up at compile time.

### Quick reference: do I need to rebuild firmware, companion, or both?

| Changed | Rebuild |
|---|---|
| `src/*.cpp`, `src/*.h`, `boards/**`, root `CMakeLists.txt` | Firmware only |
| `companion/src/main/**`, `companion/src/renderer/**`, `companion/src/preload.ts` | Companion only |
| `companion/src/shared/protocol.ts` or `companion/src/shared/types.ts` | Companion (always) + firmware if the corresponding C++ side (`companion.cpp`'s `CommandId` enum, `kProtocolMinor`, report layout) also changed — check whether you edited both before assuming one side is enough |
| `src/companion.cpp` | Firmware always; companion app only if you also changed `protocol.ts`/`types.ts` to match |

## Diagnostics: how to see what went wrong during a smoke test

If a smoke test fails (Wi-Fi doesn't connect, WOL packet doesn't send/wake
the PC, controller behaves oddly with WOL enabled), there are two separate
places to look, since firmware and companion are different processes with
different logging paths.

### Firmware logs (the important one for WOL issues)

`DS5_LOG(...)` calls (including every `[WOL]`-prefixed message in
`wolwifi.cpp`) are **compiled out entirely** unless the firmware is built
with debug logging enabled — the default release build (what
`boards/build_waveshare_rp2350b_plus_w.sh` and our earlier smoke-test builds
produced) has them fully stripped, so nothing will show up no matter what
you enable in the companion app's UI.

To get a firmware build that actually emits `[WOL]` logs:

```bash
cmake -S . -B build/waveshare-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DWAVESHARE_RP2350B_PLUS_W_BUILD=ON \
  -DENABLE_COMPANION=ON -DDS5_DIAGNOSTICS_PRESET=custom -DENABLE_DEBUG_LOGS=ON \
  -DPICO_NO_COPRO_DIS=1 -DPICO_SDK_PATH="C:/auto/arduino/build/pico-sdk"
cmake --build build/waveshare-debug --target ds5-bridge
```

Flash `build/waveshare-debug/ds5-bridge.uf2` instead of the release build
while debugging. `DS5_DIAGNOSTICS_PRESET=custom -DENABLE_DEBUG_LOGS=ON` is
the minimal combination for just firmware logs, without the extra
audio/trigger/feedback trace overhead `traces`/`all` presets add.

These logs don't come out over a wired UART cable — they're transported
over the same companion HID channel already used for settings, and the
companion app writes them to a file on disk:

1. Open the companion app, go to the device card, switch to the
   **Diagnostics** tab (top-right toggle next to "Device").
2. Under **Firmware UART Log**, click **Choose Folder** and pick a
   directory — this both enables capture and sets where the log file lands.
   The current file path is shown right there (`File: ...`) once capture is
   running.
3. Reproduce the issue (connect the controller, etc.), then open that log
   file — it will contain every `[WOL]` line in order: init, Wi-Fi
   connecting/connected + IP, controller-connect trigger, magic packet
   sent/failed, link lost, credential/MAC updates, etc. (see `wolwifi.cpp`
   for the exact message text at each step).
4. The same Diagnostics tab also shows **SRAM overwrite loss** (bytes) — if
   nonzero, the log ring buffer wrapped before the companion app could read
   it; reproduce the issue with fewer other things happening first, or
   expect to have missed early messages.

### Companion app issues (protocol errors, UI not updating, connection problems)

- Field-level errors (e.g. an invalid MAC/SSID/password rejected by
  firmware) surface directly in the WOL section's UI as inline red text —
  no log file needed for those.
- The companion app itself doesn't write a persistent log file. For
  anything else (HID connection drops, IPC errors, unexpected exceptions),
  run it via `npm run dev` instead of the installed build and check the
  Electron DevTools console (main window) plus the terminal `npm run dev`
  is running in (main-process `console.error` output lands there).
- The Diagnostics tab's other fields (Protocol, Last ACK, Revision,
  Settings Revision) are useful for confirming the companion app and
  firmware are actually talking and agreeing on protocol version — relevant
  if `wolControl`/other WOL UI controls appear disabled unexpectedly (see
  the protocol-minor gate decision in `DECISIONS.md`).

## Local build environment notes

- On this machine, `arm-none-eabi-objdump`/the chained CMake `POST_BUILD`
  step that generates `.dis`/`.hex`/`.bin`/`.uf2` sometimes crashes
  (`Access violation`) when invoked through Ninja's nested `cmd.exe` chain.
  This is NOT related to firmware source changes — `objdump`, `objcopy`, and
  `picotool` all succeed when run manually against the same `.elf` (e.g. via
  PowerShell) right after a failed build. It's also intermittent, not 100%
  reproducible — the same target can crash on one invocation and link clean
  on the next with no source changes in between. If `cmake --build` fails
  only at the final post-link step after "Linking CXX executable
  `<target>.elf`" (for the main firmware: after "Verified complete live
  firmware hot paths..." too), the actual compile+link succeeded — just
  re-run the objdump/objcopy/picotool commands from the failing command line
  manually (or via PowerShell) to produce the missing artifacts. Example
  (adjust paths/target/family per build):
  ```powershell
  $ArmGcc = "C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin"
  $Elf = "<path from the failing command>\<target>.elf"
  Set-Location (Split-Path $Elf)
  & "$ArmGcc\arm-none-eabi-objcopy.exe" -Oihex $Elf "<target>.hex"
  & "$ArmGcc\arm-none-eabi-objcopy.exe" -Obinary $Elf "<target>.bin"
  & ".\_deps\picotool\picotool.exe" uf2 convert --quiet $Elf "<target>.uf2" --family <rp2040|rp2350-arm-s>
  ```
- **This crash hits every Pico target built on this machine, not just the
  main `ds5-bridge` firmware** — confirmed 2026-08-15 in
  `tools/build-pico-universal-flash-nuke.ps1` too (invoked by `npm run
  installer:win`/`package:win`/`package:win:local` via `build:firmware-tools`
  before it does anything companion-app-specific). That script builds
  **two** separate targets (`rp2040` board, `rp2350`/pico2 board) each via
  their own `cmake --build ... --target flash_nuke`, then combines both
  UF2s into `companion/firmware/pico-universal-flash-nuke.uf2` and writes a
  SHA-256 into both a `.sha256` file and
  `companion/src/main/pico-universal-flash-nuke-hash.ts`. If either target's
  post-link step crashes, the whole `npm run installer:win`/etc. invocation
  fails before reaching `electron-builder`, and any packaged companion app
  you already had stays stale (see the next bullet for why that's a trap).
  Workaround: build+workaround each target's `.uf2` individually as above,
  then either retry the full `npm run installer:win` (Ninja may no-op the
  already-built targets and get further this time — it did once and crashed
  again on a fresh relink attempt, so this isn't guaranteed), or reproduce
  the script's final combine+hash step by hand (`companion/firmware/pico-
  universal-flash-nuke.uf2` = `rp2040/flash_nuke.uf2` bytes followed by
  `rp2350/flash_nuke.uf2` bytes, then SHA-256 the combined file into both
  destinations) and run `npm run build && npx electron-builder --win nsis
  --x64` (installer) / `npm run build && node scripts/package-win.mjs
  "<out dir>"` (portable/local copy) directly, skipping
  `build:firmware-tools` since its output is already in place.
- `PICO_NO_COPRO_DIS=1` avoids a separate, reproducible `picotool coprodis`
  segfault when it processes `bs2_default.dis` in-place; pass it when
  configuring if the boot_stage2 disassembly step crashes.
- **A `build/` (or `build-*/`) directory configured from a different
  worktree/checkout path errors immediately** ("The current
  CMakeCache.txt directory ... is different than the directory ... where
  CMakeCache.txt was created") — this repo has lived at more than one local
  path (e.g. `c:/game/DS5_Bridge` before `c:/auto/arduino/DS5_Bridge`), and
  every gitignored build directory (`build/waveshare`, `build/default`,
  `build/pico-universal-flash-nuke/*`, `build-firmware-tests`, etc.) can
  independently carry this stale-path cache. Safe fix: `rm -rf` just that
  one build directory and reconfigure — these are pure build output, never
  source.
- **After a `PROTOCOL_MAJOR`/`PROTOCOL_MINOR` bump (own work or an upstream
  merge), reflashing the firmware is only half the fix.** The companion app
  checks the flashed firmware's protocol version against its own compiled-in
  `PROTOCOL_MINOR` and shows "Update required: Bridge Settings > Firmware"
  on any mismatch — so an *installed/packaged* companion `.exe`
  (`companion/artifacts/installer/win-unpacked/DS5 Bridge.exe`, an NSIS
  install, or a `package:win:local` copy) built before the bump will show
  this error even though the newly-flashed firmware is completely correct.
  Rebuilding firmware and rebuilding/repackaging the companion app are two
  separate steps — always do both after a protocol version change, not just
  whichever one you happened to be editing. `npm run dev` (unpacked,
  runs from source) doesn't have this trap since it's never stale by
  construction; only pre-built/packaged `.exe` copies can drift.

## Rebase/backport workflow

When upstream `port-dev`/`main` moves forward (e.g. the next release after
v1.7.0):

1. Fetch `upstream/main` and `upstream/port-dev` (check which one is ahead
   — as of the v1.7.0 merge they'd converged to the same tip, but that's not
   guaranteed to stay true).
2. Merge (this fork has used merge, not rebase, for the v1.7.0 sync — see
   `CHANGELOG.md`'s 2026-08-15 entry) our feature branch onto the new tip.
3. Because WOL lives mostly in `wolwifi.h/.cpp` (firmware) and isolated
   companion-app additions, conflicts should be limited to the few hook
   points documented in `DECISIONS.md` (e.g. the `bt.cpp` connect-event call
   site, `CMakeLists.txt` option block, `protocol.ts`/`companion.cpp`
   COMMAND_ID lists).
4. **Always check the `COMMAND_ID` lists for numeric collisions, even if
   the merge reports no conflict on those lines.** Git can't detect two
   independently-added enum values landing on the same number — this
   happened for real at v1.7.0 (`SET_WOL_ENABLED` vs upstream's
   `SET_RADIAL_DEADZONES`, both at `0x37`; see `DECISIONS.md`). Grep both
   `protocol.ts`'s `COMMAND_ID` and `companion.cpp`'s `CommandId` for every
   hex value in use, take the max across both, and renumber this fork's WOL
   IDs above it if there's any overlap. Bump `PROTOCOL_MINOR`/
   `kProtocolMinor` together in both files, and update the hardcoded
   `constexpr uint8_t kProtocolMinor = N;` string match in
   `tests/firmware/usb_descriptor_migration_test.cpp` to the new value —
   that test will fail (correctly) if you forget.
5. Rebuild and test both the firmware (all build targets, including the
   default non-WOL board) and the companion app (`npm run test:companion`,
   `npm run test:firmware`) — see "Quick reference: do I need to rebuild
   firmware, companion, or both?" above. Reflash real hardware and rebuild/
   repackage whichever companion `.exe` you actually run before calling it
   done (see "Local build environment notes" — a protocol bump needs both
   sides rebuilt, not just the one you were editing).
6. If updating the existing upstream PR rather than opening a new one:
   force-push is expected (the PR history gets rebuilt on the new base) —
   confirm with the user first since it rewrites already-published history.
   Re-check the PR description afterward for anything now-stale (file lists,
   test counts, "want a squash?"-type notes that may already be resolved).

## Working style

- Research before implementing; do not assume a referenced PR (e.g. upstream
  PR #93) applies cleanly without verifying against the current branch.
- Confirm non-trivial architecture decisions with the user before committing
  to them (see DECISIONS.md for the log of decisions already made).
- Keep commits logically separated (firmware Wi-Fi/WOL, companion app config,
  build integration) rather than one large commit.
