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

- **task.md** — current + next task, organized by phase of the active plan.
  Updated as work progresses within a plan. Cleared and reset when a plan
  completes (its summary moves to changelog.md).
- **changelog.md** — durable record of completed plans/phases, newest first.
- **decisions.md** — architecture/technical decisions made along the way,
  with rationale, so later rebases or PR reviewers understand *why*.
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

- Push when a phase (or a meaningful chunk of one) from task.md is complete
  and builds/tests cleanly — e.g. after Wi-Fi bring-up compiles, after the
  companion app changes are wired end-to-end, after a smoke-test round passes.
- Push before ending a work session, so nothing is stranded only in the local
  worktree.
- Prefer pushing a branch (`feature/wol-wifi`) over pushing to a shared
  branch; never force-push without explicit confirmation.
- Commit locally frequently (small, logically separated commits per
  decisions.md/task.md phase breakdown) — pushing is about syncing to GitHub,
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
- Both `companion/artifacts` and `build`/`build-*` are already gitignored at
  the repo root; nothing extra was needed there.

If a future version of this feature needs a genuinely new output location
(e.g. bundling the Waveshare UF2 into the companion installer the way
`pico-universal-flash-nuke.uf2` is bundled via `companion/firmware/` +
`extraResources` in `package.json`), record that decision in `decisions.md`
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
  the protocol-minor gate decision in `decisions.md`).

## Local build environment notes

- On this machine, `arm-none-eabi-objdump`/the chained CMake `POST_BUILD`
  step that generates `ds5-bridge.dis`/`.hex`/`.bin`/`.uf2` sometimes crashes
  (`Access violation`) when invoked through Ninja's nested `cmd.exe` chain.
  This is NOT related to firmware source changes — `objdump`, `objcopy`, and
  `picotool` all succeed when run manually against the same `.elf` (e.g. via
  PowerShell) right after a failed build. If `cmake --build` fails only at
  the final post-link step after "Linking CXX executable ds5-bridge.elf" and
  "Verified complete live firmware hot paths...", the actual firmware build
  succeeded — just re-run the objdump/objcopy/picotool commands from the
  failing command line manually (or via PowerShell) to produce the missing
  `.uf2`/`.hex`/`.bin`/`.dis` artifacts.
- `PICO_NO_COPRO_DIS=1` avoids a separate, reproducible `picotool coprodis`
  segfault when it processes `bs2_default.dis` in-place; pass it when
  configuring if the boot_stage2 disassembly step crashes.

## Rebase/backport workflow

When upstream `port-dev` moves forward:

1. Fetch `upstream/port-dev`.
2. Rebase (or merge) our feature branch onto the new `port-dev` tip.
3. Because WOL lives mostly in `wolwifi.h/.cpp` (firmware) and isolated
   companion-app additions, conflicts should be limited to the few hook
   points documented in `decisions.md` (e.g. the `bt.cpp` connect-event call
   site, `CMakeLists.txt` option block, `protocol.ts` COMMAND_ID list).
4. Re-run the smoke tests in task.md/changelog.md before re-opening or
   updating the PR.

## Working style

- Research before implementing; do not assume a referenced PR (e.g. upstream
  PR #93) applies cleanly without verifying against the current branch.
- Confirm non-trivial architecture decisions with the user before committing
  to them (see decisions.md for the log of decisions already made).
- Keep commits logically separated (firmware Wi-Fi/WOL, companion app config,
  build integration) rather than one large commit.
