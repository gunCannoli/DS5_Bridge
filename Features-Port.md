# Features-Port.md: carrying this fork's custom features onto a new upstream release

Reusable runbook. Read it top to bottom at the start of every upstream sync,
then update **§2 (status table)** and **§9 (release log)** when you finish.
`agents/CUSTOM.md` stays the detailed per-feature reference (exact hook
snippets, wire formats). This file answers: *what do we carry, what is still
ours, and in what order do we re-apply it.*

First analysis: **upstream v2.0.1** (release tag `v2.0.1`, protocol minor 24),
ported from fork base `v1.7.1` (protocol minor 20). Do not write issue or pull
request references in this file (see `agents/CONTRIBUTING-FORK.md`).

---

## 1. Strategy: port forward, do not merge

v1.7.1 to v2.0.1 touches ~465 files (+76k/-21k lines). The companion renderer
was rebuilt (`App.tsx` split into `renderer/features/*`), the firmware moved
to run from SRAM with patched CYW43 drivers, and upstream shipped its own
Wake-on-LAN. A `git merge` would conflict almost everywhere and would drag our
`wolwifi` code back in next to theirs.

**Do this instead:**

1. `git fetch upstream --tags`
2. `git switch -c port/<tag> <tag>` (branch straight off the upstream tag).
3. Re-apply only what §2 says is still ours, as small separate commits, one
   per feature (so each can still become an upstream PR).
4. Keep the old branches (`feature/wol-wifi`, `pr/*`) untouched as the
   reference for "what the code looked like on the previous base".
5. When the port builds and passes smoke tests, make `port/<tag>` the new
   integration branch. Only then update `agents/` docs.

Everything in `agents/` is fork-local. Copy `agents/`, `AGENTS.md`, `tools/`,
and this file onto the new branch first; they are not upstream files.

---

## 2. Status table (v2.0.1)

| # | Custom feature | Where | Status on v2.0.1 | Action |
|---|---|---|---|---|
| 1 | Wake-on-LAN over Wi-Fi (`wolwifi`) | firmware + companion | **Upstream now ships its own.** Different design (§3). | **Drop ours, adopt upstream's.** Keep only the §3.4 follow-ups if smoke tests show a gap. |
| 2 | Auto Switch Audio on Jack | companion only | Not upstream. Upstream still only *parses* `headsetPlugged`. | **Re-apply** (§4). Renderer hook points moved. |
| 3 | Audio-aware Idle Disconnect | `src/bt.cpp`, one `if` | Not upstream. Idle block unchanged (`if (mute[1]) return;` then the timeout test). | **Re-apply** (§5). Trivial. |
| 4 | Display-aware Idle Disconnect | companion (`DisplayStateMonitor.cs` + bridge-service) | Not upstream (no display-state code found). Never analysed for this port. | **Re-apply, after #2** (§6). Read commit `63da3bb` first. |
| 5 | Speaker OUT FIFO 3 to 8 packets | `src/tusb_config.h` (1 line) | Upstream still uses `3 *`. Audio path changed in 2.0. | **Re-test before porting** (§7). May be unnecessary. |
| 6 | `vreg_set_voltage(DEFAULT)` on <=150 MHz builds | `src/main.cpp` (uncommitted, LOCAL-ONLY) | Upstream `main.cpp` still has the `#if SYS_CLOCK_KHZ != 150000` block with no `#else`. | Commit it on the old branch first, then re-apply (§7). |
| 7 | Uncommitted `HoldsAllDefaultRenderRoles` skip | `EndpointManager.cs` (LOCAL-ONLY) | Belongs to #2. | Commit on the old branch, carry with #2. |
| 9 | WOL lightbar indicator | firmware, optional | Not upstream. See §7a. | Decide after the WOL smoke test. |
| 8 | Dev tooling | `tools/*.ps1`, `boards/run_firmware_tests.sh`, `package:win:local` | Needs compat check (§8). | Re-apply last. |

The old `smoke` firmware variant (`-DWOL_ALWAYS=ON`) and the `WOL_ALWAYS` /
`ENABLE_WOLWIFI` options **disappear** with #1. Remove that variant from
`tools/build-firmware.ps1`.

---

## 3. Wake-on-LAN: ours vs upstream, and the "keep only one" decision

### 3.1 Comparison

| | Ours (`wolwifi`) | Upstream 2.0 (`wake_on_lan*`) |
|---|---|---|
| Origin | Written here | Ported from Kitsune Input (their release notes say so) |
| Packet | UDP magic packet through lwIP (DHCP, ARP, unicast/broadcast) | **Raw Ethernet frame, EtherType `0x0842`, no IP stack** |
| Needs IP/DHCP | Yes | No (joins WPA2 only) |
| Wake order | Wi-Fi magic packet only | **USB remote wakeup first**, then Wi-Fi packet |
| Trigger | Any controller connect (`finish_hid_session_if_ready`) gated by USB host-alive | Only an **encrypted reconnect of a bonded** controller, after the bridge *armed* (host gone 3 s, quiet 1 s, 45 s since last attempt) |
| Host-alive check | `usb_host_active()` sampled by `ObserveHost`; pre-joins Wi-Fi while host is down | Arming state machine; BIOS/UEFI sessions detected via HID boot protocol |
| Radio coexistence | lwIP + `pico_cyw43_arch_lwip_poll`, DHCP ARP-check off | **Quiet gate** (scans off, input/output/audio dropped) + hash-pinned CYW43 driver patches; reboots if Wi-Fi teardown cannot be verified |
| Config storage | BTstack TLV tag `'WOLC'` | Own A/B flash sectors below the BTstack bank, CRC + generation |
| Config path | 4 simple commands `0x46`-`0x49` | One command `0x57`, transactional (begin/write/test/commit), Test button, status report `0x1d`, **Wi-Fi country code**, password never read back |
| Companion UI | Section in Bridge Settings | Settings > Wake PC, with Windows readiness checklist (adapter WoL, Fast Startup, Modern Standby) via AudioHelper verbs |
| Lightbar feedback | Green pulse (`bt_wol_indicator_*`) | None found; verify on hardware |
| Build gate | `ENABLE_WOLWIFI` (default on for Waveshare) | `ENABLE_COMPANION` (already on for our builds) |
| Tests | 2 companion tests | Engine, store, CYW43 patch and wire-format fixtures, companion tests |

Checked in the v2.0.1 source (not on hardware):

- **Lightbar:** no lightbar/LED call anywhere in `wake_on_lan*.cpp` or in the
  WOL-related `bt.cpp` paths. Progress shows only in the companion (Wake PC
  stage: waiting, armed, joining, sending, stopping Wi-Fi, failed). While Wi-Fi
  owns the radio, the quiet gate drops controller output, so the lightbar
  cannot be updated at that moment either.
- **Boot survival:** designed in. After a submitted packet the engine sets
  `host_starting_` for up to 5 min (`kHostStartingCapMs`) or until the host
  has been enumerated 45 s (`kHostReturnMs`). BIOS enumerating then dropping
  the bridge does not count as a returned host. During that window
  `suspend_exempt()` stops the sleep-disconnect policy from powering the
  controller off. Needs the hardware smoke test to confirm.
- **Cost when idle:** one always-inlined load of `wake_on_lan_quiet_cached` in
  the SRAM paths; the WLAN work runs only during an attempt.

### 3.2 Decision: keep upstream's, drop ours

Reasons:

- It is the maintained path. Carrying a second WOL stack means every upstream
  CYW43/BT change has to be reconciled with an lwIP stack the 2.0 firmware
  was not designed around (SRAM-resident hot paths, the quiet gate, the
  hash-pinned driver patches).
- Both features cannot coexist: upstream replaces the CYW43 control source
  and links the non-lwIP arch; ours needs `pico_cyw43_arch_lwip_poll`.
- Upstream adds safeguards ours lacks (teardown verification, USB-first wake,
  readiness checklist).

### 3.3 What to remove from our tree (do not re-apply)

Firmware: `src/wolwifi.{h,cpp}`, `boards/headers/lwipopts.h` (check nothing
else uses it), the `ENABLE_WOLWIFI`/`WOL_ALWAYS` CMake blocks, the
`bt.cpp` connect hook and `bt_wol_indicator_*`, `usb_host_active()` and the
`wolwifi_wake_in_progress()` power-off guard, `WatchdogMainLoopPhase::Wolwifi`,
and the `0x46`-`0x49` commands in `companion.cpp`.

Companion: the four `wol*` settings, `SET_WOL_*` IDs, `wolControl` gate,
`setWol*` IPC, and the Wake-on-LAN section in `App.tsx`.

Upstream's WOL files arrive with the tag, so there is nothing to copy.
Command IDs: upstream's highest is `0x57`; ours (`0x46`-`0x49`) vanish, so no
renumbering is needed on this port.

### 3.4 Smoke test and fallbacks (do before deleting the old branch)

Hardware test on the Waveshare board, wired PC, PC fully shut down:

1. Settings > Wake PC: pick adapter, SSID, password, **country**, run **Test**.
2. Turn the controller off, shut the PC down, turn the controller on. Expect
   wake, then a clean reconnect once the PC is up.
3. Repeat from sleep, from hibernate, and with the PC already on (no packet).

Known behavioural differences to judge, not assume:

- **Raw `0x0842` frames over Wi-Fi.** Some access points/mesh nodes do not
  forward unknown EtherTypes to the wired side. Our UDP packet worked on this
  network; if the raw frame does not arrive, that is an upstream limitation to
  report, not something to bolt our lwIP path back on.
- **Setup requires no controller link and an active host.** Credentials can
  only be set with the controller off.
- **Existing credentials do not migrate.** Re-enter them in Wake PC. Our old
  `'WOLC'` TLV entry in the BTstack bank is harmless; leave it.
- **No green lightbar pulse** unless hardware shows otherwise. If we miss it,
  it is a cosmetic, separate follow-up (a small PR), not a reason to keep the
  old stack.
- The "controller dropped while the PC booted" issue from the old
  investigation (see `agents/DECISIONS.md`) was in our stack; re-check only
  that scenario on the new one.

Only if the upstream path fails on this network after troubleshooting, reopen
the question of carrying `wolwifi` as a patch, and record the decision in
`agents/DECISIONS.md`.

---

## 4. Auto Switch Audio on Jack (companion only)

Reference: `agents/CUSTOM.md` §10 and `agents/PLAN-headset-audio-switch.md`.
Source commits on the old base: `89d59a9`, `938ebce`, `ff08d58`, `4c2ba3d`,
`d77d610` (the last four are fixes that the earlier PR branch predates), plus
the uncommitted `HoldsAllDefaultRenderRoles` skip (row 7).

Re-apply map for v2.0.1 (file names verified present on the tag):

| Piece | Target on v2.0.1 | Notes |
|---|---|---|
| AudioHelper verbs `--set-default-render`, `--list-render-endpoints`, `IsRemoteSession`, clean-exit wrapper | `companion/native/AudioHelper/EndpointManager.cs`, `Program.cs` | Upstream still has `--default-render-status` / `--set-default-render-bridge`. `Program.cs` arg parsing moved (now ~line 2900+); add cases next to those two. Upstream added `--list-physical-ethernet-adapters` / `--wake-on-lan-readiness`; do not disturb them. |
| TS wrappers | `companion/src/main/audio-helper.ts` | Add `setDefaultRenderEndpointByName`, `listRenderEndpoints`, `RenderEndpointInfo`. |
| Settings fields | `shared/types.ts`, `main/settings-store.ts` | **New in 2.0: per-bridge settings workspaces** (`settings-bridge-workspaces.ts`). Decide whether these two fields are global or per-bridge. They were global on v1.7.1; keep global unless the workspace code forces otherwise. |
| Poll hook + feature block | `main/bridge-service.ts` | Hook after `syncControllerPowerSavingState` in `poll()` (method still exists, plus `hostPersonaDefaultRenderRestore`, `setDefaultRenderBridgeEndpoint`, `closeDevice`). Re-read each, since 2.0 changed persona-switch behaviour. |
| IPC | `main/main.ts`, `preload.ts` | 3 channels. Check the new IPC contract test style before adding. |
| UI | `renderer/features/settings/ControllerBridgeSettings.tsx` (`ConnectionPowerSettings`) | Replaces the old `App.tsx` "Power & Controller" column. New rows must use `SearchableSetting` + a stable id so the settings search finds them. Fallback dropdown uses `components/CustomSelect`. |
| Strings | `renderer/common/localization/catalogs.ts` | Eight more languages now exist. Check whether a missing key fails a test or falls back to English before deciding how much to translate. |
| CSS | feature stylesheet, not `styles.css` | **Drop** the full-window preferences modal and the LOCAL-ONLY `:disabled` block: the settings UI no longer has that modal. |
| Tests | `bridge-service.test.ts`, `ipc-contract.test.ts` | Reuse the `describe('Auto Switch Audio on Jack')` block (6 tests + the baseline fixture field). |

Signal check on the new firmware: `headsetPlugged` is still
`(routeFlags & 0x01)` in `protocol.ts`. Re-verify the firmware side still
passes input byte 53 bit 0 through unfiltered before relying on the debounce
assumptions.

Verify: `npm run typecheck`, `npx vitest run src`, `npm run build:audio-helper`,
`tools\rebuild-companion.ps1`, then the smoke test in
`PLAN-headset-audio-switch.md` §7 (plug/unplug, controller powered off with
headset in, RDP in/out).

---

## 5. Audio-aware Idle Disconnect (firmware, one `if`)

Reference: `agents/CUSTOM.md` §11, commit `e0b679b`.

On v2.0.1 `src/bt.cpp` the block is intact (~line 4247):

```cpp
// Inactivity detection.
if (mute[1]) { return; }
// ---- ADD (4 lines, see CUSTOM.md 11.1 for the comment text) ----
if (audio_output_route_protected()) { inactive_time = now_us; return; }
// ----
if (!meaningful_input_activity && now_us - inactive_time > idle_disconnect_timeout_us()) {
```

`audio_output_route_protected()` still exists (forward decl ~416, definition
~5077). Then restore the matching assertions in
`tests/firmware/usb_descriptor_migration_test.cpp` (that file changed by ~340
lines in 2.0; re-add the assertion block, do not copy the old file).
Rebuild firmware only.

---

## 6. Display-aware Idle Disconnect (companion)

Source: commit `63da3bb` (and `agents/PLAN-idle-disconnect-presence.md`).
New file `companion/native/AudioHelper/DisplayStateMonitor.cs` plus hooks in
`Program.cs`, `audio-helper.ts`, `bridge-service.ts`, settings, IPC, and UI.
Not yet analysed against 2.0. Before porting:

1. Read the commit diff and list its hook points (mirror §4's table).
2. Check upstream's `ControllerBridgeSettings.tsx` Idle Disconnect row; the
   UI addition belongs there.
3. Port after §4 so both share the same AudioHelper/IPC edits.

---

## 7. Small firmware items

- **Speaker FIFO (`tusb_config.h`).** `PLAN-audio-stutter.md` records this
  area as investigated; the 8-packet change landed on the test branch. 2.0
  rewrote the audio/USB path, so first flash stock v2.0.1 firmware and see
  whether the stutter even reproduces. Port the one-liner only if it does.
- **vreg reset (`main.cpp`).** Commit the LOCAL-ONLY `#else` branch on the old
  branch, then add it to the same `#if SYS_CLOCK_KHZ != 150000` block on the
  new base. Mark it `LOCAL-ONLY`.

---

## 7a. Possible new custom feature: Wake-on-LAN lightbar indicator

**Status: not built.** Upstream WOL gives no on-controller feedback (§3.1), and
our old stack had a green pulse while the packet was in flight, then a solid
green hold when the PC answered. This is the one thing the switch to upstream's
WOL loses. It would be a small, separate, additive patch on top of upstream's
engine. It should become its own PR if it works, and stays `LOCAL-ONLY` until then.

Old implementation, for reference (old branch, commit `a728df2`):
`WolIndicatorPhase` enum + statics, `bt_wol_indicator_begin/confirm/cancel/loop()`
near `bt_lightbar_loop()` in `bt.cpp`, declarations in `bt.h`, and a loop call
in `main.cpp`'s Lightbar phase. It kept its **own** colour snapshot because
`bt_set_lightbar_color()` overwrites the normal restore target on every call.

Constraints that are new in 2.0, to design around:

- **The quiet gate blocks the lightbar while Wi-Fi is active.** During
  Joining/Sending/Teardown, `wake_on_lan_quiet_gate()` drops controller output
  (`bt.cpp` ~4589 and ~5467). A pulse during the join is impossible unless the
  indicator is exempted, which touches the gate's safety design. Do not do
  that. Prefer showing it *around* the Wi-Fi window instead.
- **Candidate signals** (no code change in the engine needed for the first):
  1. `wake_on_lan_armed()` rising edge to "armed" (dim steady colour), only
     if that is wanted at all.
  2. Controller reconnect after the packet while the host is still starting
     (`host_starting_` is private, so this needs a tiny public accessor in
     `wake_on_lan.h`, e.g. `wake_on_lan_host_starting()`): pulse green until
     the host enumerates, then a brief solid green and restore.
  3. Failure (`Stage::Failed`): one short red/amber flash, if it is worth it.
- **Hook points:** one accessor in `wake_on_lan.{h,cpp}`, the indicator block
  in `bt.cpp`/`bt.h`, one loop call in `main.cpp` next to `bt_lightbar_loop()`,
  and a poll of the accessor from `service_wake_on_lan()`.
- **Cost:** a few dozen lines, no protocol or companion change, no new flash
  data. Must run outside the SRAM hot paths (cold code only).

Decide after the §3.4 smoke test whether the missing feedback actually hurts.

## 8. Build and tooling after the port

- **Pico SDK:** upstream's CYW43 patches are hash-pinned and fail closed on any
  other version. The installed `C:\auto\arduino\build\pico-sdk` is **2.3.0**,
  which matches. Re-check on every release (`sdkVersion` near the top of
  `CMakeLists.txt`).
- **Waveshare gate:** `WAVESHARE_RP2350B_PLUS_W_BUILD` still exists in
  `CMakeLists.txt` and `boards/build_waveshare_rp2350b_plus_w.sh` ships.
- **Companion build is mandatory for WOL.** Upstream's WOL compiles only with
  `ENABLE_COMPANION`; `tools/build-firmware.ps1` already passes it.
- Update `tools/build-firmware.ps1`: remove the `smoke` variant and any
  `ENABLE_WOLWIFI`/`WOL_ALWAYS` arguments; bump the `-wol-` part of the staged
  file name or rename it (nothing WOL-specific is ours anymore).
- `companion/scripts/package-win.mjs` and `package:win:local` must be
  re-diffed; 2.0 changed packaging.
- `boards/run_firmware_tests.sh` points at `tests/firmware/*`, which gained
  several suites and CMake-script tests in 2.0. Run it and fix the runner, not
  the tests.
- Fix `agents/` docs last: `CUSTOM.md` (features 1 removed), `DECISIONS.md`
  (WOL entries become historical), `TASK.md`, `CHANGELOG.md`, `AGENTS.md`
  (WOL purpose paragraph, `-wol-` file names, smoke variant).

---

## 9. Generic checklist for any future release

1. `git fetch upstream --tags`; read the release notes for features we carry.
   If upstream added one, run §3-style comparison and prefer theirs.
2. Branch off the tag (§1). Copy `agents/`, `AGENTS.md`, `tools/`, this file.
3. For each row in §2, grep the tag to see if it moved or landed upstream:
   ```
   git grep -n "audio_output_route_protected" <tag> -- src/bt.cpp
   git grep -nE "set-default-render|list-render-endpoints" <tag> -- companion
   git grep -niE "wake|wol" <tag> -- src CMakeLists.txt
   git ls-tree -r --name-only <tag> | grep -iE "wol|wake|display"
   ```
4. Command IDs: if any custom command still exists, list every hex value in
   `companion.cpp` `CommandId` and `protocol.ts` `COMMAND_ID`, take the max,
   renumber ours above it in both files together, and bump `kProtocolMinor` /
   `PROTOCOL_MINOR` consistently (and the string assertion in
   `usb_descriptor_migration_test.cpp`). Currently we have **no custom
   commands**.
5. Re-apply features in this order: firmware one-liners (§5, §7), then
   companion (§4, §6), then tooling (§8).
6. Check the SDK pin, run host tests, typecheck, vitest, build both sides.
7. Flash and smoke test (WOL, jack switch, idle disconnect), then update §2
   and add a §10 entry.

---

## 10. Release log

| Release | Date | Outcome |
|---|---|---|
| v2.0.1 | 2026-10-29 | Port in progress on branch `port/v2.0.1` (off the tag). Done: WOL switched to upstream's (ours not carried); audio-aware idle disconnect (`bt.cpp` + test); regulator reset; Auto Switch Audio on Jack (AudioHelper, service, IPC, new settings row, all 8 locale catalogs); display-aware idle disconnect; fork tooling (`build-firmware.ps1` without `smoke`, `package:win:local`). Host firmware tests, companion typecheck and 865 vitest tests pass; firmware `final` builds. Pending: hardware smoke test (WOL, jack switch, idle), FIFO re-test (§7), lightbar decision (§7a). |

---

## 11. Open questions for the first port

1. Does upstream's raw-frame WOL wake this PC through the current AP/router?
   (§3.4. Decides whether #1 closes.)
2. Does the speaker stutter reproduce on stock v2.0.1? (§7.)
3. Should the two Auto Switch Audio fields be global or per-bridge under the
   new workspaces? (§4.)
4. How much of the new localization catalogs must be filled for new strings?
   (§4.)
5. Do we still want to pursue upstream PRs for features 2 and 3, now that the
   base moved? Rebase them from the new branch if so.
