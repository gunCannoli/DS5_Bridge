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
| 9 | WOL lightbar indicator | firmware | Not upstream. **Built 2026-11-07** (§7a). | **Re-apply** (§7a, 5 hook points). Hardware test pending. |
| 10 | WOL UDP-broadcast magic frame | firmware | Not upstream. Upstream's raw `0x0842` frame did not wake this PC through this network. **Built 2026-11-07** (§7b). | **Re-apply** (§7b, 2 hook points). Re-test whether upstream's frame works first on each release. |
| 11 | Reboot when CYW43 fails to start | `src/main.cpp` (one block) | Not upstream. `main()` still does `return 1` and hangs. Ported from DS5Dongle 0.8.0. | **Unpublished PR** (§7c.1). |
| 12 | SMPS PWM mode (coil whine) | `src/main.cpp` + Waveshare board header | Not upstream. Ported from DS5Dongle 0.8.0. | **Unpublished PR** (§7c.2). |
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
| Lightbar feedback | Green pulse (`bt_wol_indicator_*`) | None upstream; we add it again as a small patch (§7a) |
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
- **No green lightbar pulse upstream.** We add it back as a small patch (§7a),
  not by keeping the old stack. The raw-frame delivery problem is handled by
  §7b.
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
| Poll hook + feature block | `main/bridge-service.ts` | Hook after `syncControllerPowerSavingState` in `poll()` (method still exists, plus `hostPersonaDefaultRenderRestore`, `setDefaultRenderBridgeEndpoint`, `closeDevice`). Re-read each, since 2.0 changed persona-switch behaviour. **Ensure both verify windows (`PLUG_VERIFY_WINDOW_MS` and `UNPLUG_VERIFY_WINDOW_MS`) are bounded to 30 s so steady-state polling never spawns background processes.** |
| IPC | `main/main.ts`, `preload.ts` | 3 channels. Check the new IPC contract test style before adding. |
| UI | `renderer/features/settings/ControllerBridgeSettings.tsx` (`ConnectionPowerSettings`) | Replaces the old `App.tsx` "Power & Controller" column. New rows must use `SearchableSetting` + a stable id so the settings search finds them. Fallback dropdown uses `components/CustomSelect`. |
| Strings | `renderer/common/localization/catalogs.ts` | Eight more languages now exist. Check whether a missing key fails a test or falls back to English before deciding how much to translate. |
| CSS | feature stylesheet, not `styles.css` | **Drop** the full-window preferences modal and the LOCAL-ONLY `:disabled` block: the settings UI no longer has that modal. |
| Tests | `bridge-service.test.ts`, `ipc-contract.test.ts` | Reuse the `describe('Auto Switch Audio on Jack')` block (6 tests + the baseline fixture field). |

**Connect-time and performance fixes (2026-10-31, found on v2.0.1; carry them with the port).**
Plugging the headset in *before* the controller pairs did not switch audio, and
an unbounded empty-jack verification loop caused micro-stuttering.
Four causes, four changes, all in `syncHeadsetAudioAutoSwitch()`:

1. **Plug does not need a fallback.** With the fallback on Auto and 0 or 2+
   other outputs (TV plus Steam Streaming Speakers) the old early return
   idled the whole feature. Now only the *empty-jack* direction needs a
   resolved fallback; plugging always routes to the controller.
2. **Wait for `audioStatus.controllerStateReady`.** Right after pairing the
   firmware has no input report yet, so the jack bit reads empty. Do not
   evaluate (and reset the pending/plugged-since state) until it is ready.
3. **Re-verify after a plug.** For `HEADSET_AUDIO_PLUG_VERIFY_WINDOW_MS`
   (30 s) after the plug, every 2 s check the default render endpoint and
   re-apply the controller if Windows moved it
   (`correctDefaultRenderIfNotBridgeWhileJackPlugged`). Bounded so a manual
   output change later is not fought.
4. **Bounded empty-jack verification window (CRITICAL FOR PERFORMANCE):**
   Previously, while the jack was empty (`!jackPlugged`), `correctDefaultRenderIfBridgeWhileJackEmpty`
   polled every 2 s indefinitely, spawning `AudioHelper.exe` and `conhost.exe 0x4`
   continuously in the background and causing desktop/game micro-stuttering.
   Now bounded by `HEADSET_AUDIO_UNPLUG_VERIFY_WINDOW_MS` (30 s) via `headsetJackUnpluggedSince`
   (matching the plug verification window). After the 30-second window following
   connect or an unplug event, background process polling drops to **0 checks per second**.
   In steady state, **zero** helper processes are spawned, completely eliminating
   `conhost.exe` process churn and CPU wakeups.

Tests: the `Auto Switch Audio on Jack` fixtures must pass
`controllerStateReady: true` to `audioStatusReport(...)`, and assert that background
polling ceases after 30 s in steady state.

**Known failure mode: Rapid AudioHelper.exe respawn & conhost churn (2026-10-31, resolved):**
- **Symptom:** Persistent background process churn, frequent `conhost.exe 0x4`
  creation, and micro-stuttering on the PC every 2–3 seconds while `DS5 Bridge.exe`
  runs in the background.
- **Root cause:**
  - The companion loop polled every 2 seconds (`HEADSET_AUDIO_EMPTY_JACK_CHECK_INTERVAL_MS = 2000`)
    indefinitely while the jack was empty (`correctDefaultRenderIfBridgeWhileJackEmpty`),
    unlike the plugged verification window which was bounded to 30 s.
  - Spawning `AudioHelper.exe` (a Windows CUI console binary) spawned a new `conhost.exe`
    process every 2 seconds throughout the entire session.
  - When the fallback endpoint was set to `'Remote Audio'` (an endpoint that only exists
    during an active Remote Desktop/RDP session), `EndpointManager.SetDefaultRenderEndpointByName`
    threw `InvalidOperationException: No active render endpoint matched any of ['Remote Audio']`,
    causing rapid retry cycles.
- **Resolution implemented in code:**
  - Symmetrically bounded the empty-jack verification window with
    `HEADSET_AUDIO_UNPLUG_VERIFY_WINDOW_MS = 30_000` using `headsetJackUnpluggedSince`.
  - In steady state (30 seconds after controller connect or a plug/unplug event), background
    polling ceases completely (0 helper invocations, 0 conhost spawns).
  - Maintained instant, reliable automatic audio switching to controller on headset plug
    and back to fallback on unplug. Unit tests in `bridge-service.test.ts` updated and passing.

Note: on v2.0.1 firmware the jack bit is *not* latched after disconnect
(`src/audio.cpp` resets `plug_headset` on disconnect); the
`&& controllerConnected` guard is kept anyway.

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

## 7a. WOL lightbar indicator (built 2026-11-07, LOCAL-ONLY)

Upstream WOL gives no on-controller feedback (§3.1). Our old stack had a green
pulse while the packet was in flight, then a solid green hold when the PC
answered. This re-creates it as a small additive patch on top of upstream's
engine. It stays `LOCAL-ONLY` until it is hardware-proven; then it can become
its own upstream PR.

**Behaviour**

| Phase | Lightbar |
|---|---|
| Packet submitted, PC not yet enumerated (`host_starting_` and no counted host) | flashing green, 500 ms on / 500 ms dark |
| PC enumerated the bridge outside its firmware (`usb_wake_host_active() && !usb_wake_host_firmware()`) | solid green |
| Engine's host-returned rule met (45 s counted), or 5 min cap | previous color restored |
| Controller disconnects, or the host takes the lightbar | stored restore color repaired, no output |

The quiet gate drops controller output during Joining/Sending/Teardown, so the
indicator starts the moment the radio is verified idle, not during the send.
Flashing green therefore means "attempt finished, PC has not answered yet", not
"packet delivered". It does not touch the quiet gate or the engine's safety
design. Cold code only, no SRAM hot-path change, no protocol or companion change.

**Files and hook points (re-apply in this order)**

1. New files `src/wake_on_lan_indicator.{h,cpp}`: the whole feature. Uses only
   `bt_set_lightbar_color`, `bt_save_lightbar_color`, `bt_get_lightbar_color`,
   `bt_is_controller_connected`, `bt_host_lightbar_owned`,
   `usb_wake_host_active`, `usb_wake_host_firmware`,
   `wake_on_lan_host_starting`.
2. `src/wake_on_lan_engine.h`: one accessor
   `bool host_starting() const { return host_starting_; }` (next to `armed()`).
   If upstream renames `host_starting_`, expose whichever flag means "packet
   sent, host not yet back" (the one `suspend_exempt()` reads).
3. `src/wake_on_lan.h` / `.cpp`: `bool wake_on_lan_host_starting()` declaration,
   the real definition in the `ENABLE_COMPANION` branch and a `return false`
   stub in the `#else` branch.
4. `src/bt.h` / `.cpp`: `bt_get_lightbar_color(...)`, a getter for the four
   `saved_lightbar_*` statics placed just above `bt_lightbar_loop()`. If those
   statics are renamed, return whatever `bt_lightbar_loop()` restores from.
5. `src/main.cpp`: `#include "wake_on_lan_indicator.h"` and one call,
   `wol_indicator_service();`, inside the existing
   `if (!wake_on_lan_quiet_gate()) { RUN_MAIN_PHASE(Lightbar, { bt_lightbar_loop(); ... }) }`
   block. If the block moved, keep the call in the same place: after the
   lightbar restore, only while the quiet gate is open.
6. Root `CMakeLists.txt`: add `src/wake_on_lan_indicator.cpp` to the base source
   list next to `src/wake_on_lan.cpp`.

**Why its own colour snapshot:** `bt_set_lightbar_color()` overwrites the normal
restore target on every call, so the indicator copies the target first and puts
it back through `bt_set_lightbar_color` (connected) or `bt_save_lightbar_color`
(disconnected), or the next connect would restore green.

**Not done (ideas):** red flash on `Stage::Failed`; a dim "armed" colour. Add
only if the hardware test shows it is needed.

## 7b. WOL UDP-broadcast magic frame (built 2026-11-07, LOCAL-ONLY)

**Problem.** On this network upstream's raw Ethernet frame (EtherType `0x0842`,
no IP layer) did not wake the PC, while a phone app sending the same MAC as a UDP
broadcast did. Many APs/routers do not forward unknown EtherTypes from a Wi-Fi
client to the wired side. (Cause inferred from the packet format; confirm with
the `debug` log and packet count if it ever matters for an upstream report.)

**Fix.** Each wake packet is sent twice back to back: upstream's raw frame
unchanged, then a finished Ethernet + IPv4 + UDP frame (144 bytes): destination
`ff:ff:ff:ff:ff:ff` / `255.255.255.255`, source IP `0.0.0.0` (as in a DHCP
discover), UDP port 9, TTL 64, valid IPv4 header checksum, UDP checksum 0, the
usual 102-byte magic payload. It still needs no IP stack, DHCP or ARP, so the
non-lwIP design and the CYW43 patches stay untouched. The send counts as
successful if either frame was accepted; upstream's engine, packet count (3) and
timing are unchanged.

**Files and hook points**

1. New files `src/wake_on_lan_udp_frame.{h,cpp}` (`magic_udp_frame`,
   `kUdpFrameSize`) and `tests/firmware/wake_on_lan_udp_frame_test.cpp`.
2. `src/wake_on_lan.cpp`, in `send()` right after the existing
   `cyw43_send_ethernet` call: build and send the UDP frame, OR the two results.
   Plus `#include "wake_on_lan_udp_frame.h"`. If `send()` changed shape, keep
   the rule: same `cyw43_send_ethernet(..., false)` call, second frame.
3. Root `CMakeLists.txt`: `src/wake_on_lan_udp_frame.cpp` in the base source list.
4. `tests/firmware/CMakeLists.txt`: the `wake_on_lan_udp_frame_test` target and
   its `add_test`. `boards/run_firmware_tests.sh` only runs three named exes, so
   run `build/waveshare-tests/wake_on_lan_udp_frame_test.exe` directly.

**Per release:** first re-test upstream's raw frame alone on this network. If a
release makes it work (or adds an IP-based packet itself), drop 7b.

**Source IP caveat:** an AP with DHCP-snooping/IP-source-guard could drop a
`0.0.0.0` source from a client with no lease. If that is ever seen, the next
step is a DHCP-less fixed source IP setting, not an lwIP stack.

**Build tooling changed with this work (fork-local):** `tools/build-firmware.ps1`
`debug` now builds the UART log only (`DS5_DIAGNOSTICS_PRESET=custom`,
`ENABLE_DEBUG_LOGS=ON`); the old `all` preset fails the link-time heap check on
v2.0.1 even without our patches. The script also no longer stages a UF2 when the
link verification fails (`verify_*:` in the build output), because its
picotool-crash recovery used to hide that.

## 7c. Upstream-candidate fixes from DS5Dongle 0.8.0 (unpublished PRs)

DS5_Bridge is derived from DS5Dongle. On 2026-11-18 we compared DS5Dongle
v0.8.0 (release notes plus the two commits below) with `port/v2.0.1`. Most of
0.8.0 was already in DS5_Bridge 2.0 in another form (button remap, chord
shortcuts, wake keyboard, link-key handling, short-frame checks). Two small
firmware fixes were missing. Each becomes its **own** PR branch. Both
branches stay **local and unpublished** (not pushed, no PR opened) until
they pass a hardware test. Each contains only its own fix, with no
`LOCAL-ONLY` code and no `agents/` changes (`CONTRIBUTING-FORK.md` §3).

| Branch | Base | Commit | Pushed | PR opened |
|---|---|---|---|---|
| `pr/cyw43-init-reboot` | `v2.0.1` tag | `c8cd697` | no | no |
| `pr/waveshare-smps-pwm` | `v2.0.1` tag | `ea0b050` | no | no |

Also carried on `port/v2.0.1`, applied as working-tree edits for now (2026-11-18).
`src/main.cpp` there also holds uncommitted §7a edits, so a cherry-pick
would have been refused. Commit the two hunks separately from the §7a ones
when `port/v2.0.1` is next committed. The `final` build passes its link
checks. The disassembly confirms both the `watchdog_reboot` path and the
`cyw43_arch_gpio_put(CYW43_WL_GPIO_SMPS_PIN, true)` call are present on
Waveshare. Host tests pass. Hardware test pending.

### 7c.1 Reboot instead of hanging when CYW43 fails to start

**Source:** DS5Dongle commit `511a34abc4` ("reboot instead of hanging when
CYW43 fails to start").

**Problem.** In `src/main.cpp`, `if (cyw43_arch_init()) { ...; return 1; }`
runs before `watchdog_enable(1000, true)`. A one-off radio start failure
leaves the bridge dead until it is unplugged. Nothing resets it, because the
watchdog is not armed yet.

**Fix (one hook point, `src/main.cpp`, the `cyw43_arch_init()` failure
branch):**

```cpp
if (cyw43_arch_init()) {
    // Returning here hangs the board (the watchdog is not armed yet) until
    // it is unplugged; reboot instead so the next boot retries.
    DS5_LOG("Failed to initialize CYW43, rebooting\n");
    sleep_ms(500);
    watchdog_reboot(0, 0, 0);
    while (true) { tight_loop_contents(); }
}
```

`hardware/watchdog.h` is already included (`watchdog_enable` is used further
down). Leave the `audio_init()` failure branch just above it unchanged: it is
a different failure and outside this PR's scope.

**Not done on purpose:** do not record a `WatchdogResetReason` for this
failure. A new enum value also needs a label in the companion's
`reset-evidence.ts`, and the telemetry snapshot is consumed *after* this
point in `main()`. Writing a reason here could overwrite the previous boot's
evidence. Leave it as a follow-up idea if a retry loop is ever seen.

**Risk.** If the radio is permanently broken, the bridge reboots every
~0.5 s instead of hanging. That still beats a silent hang and matches
DS5Dongle's behaviour.

**Test.** Host tests and a `final` build. On hardware: a normal boot is
unchanged. The failure path cannot be triggered on demand; if needed, test it
with a temporary `if (true)` that is never committed.

### 7c.2 SMPS PWM mode against coil whine

**Source:** DS5Dongle commit `23a55da7a0` ("SMPS coil-whine").

**Problem.** In power-save (PFM) mode, the board's buck regulator can whine
audibly at light load. Driving the CYW43 `WL_GPIO1` pin high forces PWM
mode: less ripple and no whine, at the cost of a few mA. That cost does not
matter on a USB-powered dongle.

**Waveshare catch.** DS5Dongle guards the call with
`#ifdef CYW43_WL_GPIO_SMPS_PIN`. The SDK's `pico_w.h`/`pico2_w.h` define it
as `1`, but our `boards/headers/waveshare_rp2350b_plus_w.h` only has the
*comment* ("the SMSP mode pin is on WL_GPIO1") and no define. Copied as-is,
the fix would do **nothing on our board**.

**Fix (two hook points):**

1. `src/main.cpp`, right after `cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);`
   (the line after the CYW43 init block):
   ```cpp
   #ifdef CYW43_WL_GPIO_SMPS_PIN
       cyw43_arch_gpio_put(CYW43_WL_GPIO_SMPS_PIN, true);
   #endif
   ```
2. `boards/headers/waveshare_rp2350b_plus_w.h`, under the existing SMPS
   comment, matching the SDK board headers:
   ```c
   #ifndef CYW43_WL_GPIO_SMPS_PIN
   #define CYW43_WL_GPIO_SMPS_PIN 1
   #endif
   ```

**Check before publishing:** confirm on Waveshare's schematic that `WL_GPIO1`
really drives the regulator's mode pin. The header comment may have been
copied from `pico2_w.h`. If it does not, drop hook point 2. The PR then fixes
only the stock Pico W / Pico 2 W builds.

**Interaction.** Upstream's Wake-on-LAN teardown verification and the
quiet gate touch the CYW43 WLAN core, not its GPIOs. The pin is set once at
boot. Verify that a Wake PC attempt (Wi-Fi up, then down) leaves `WL_GPIO1`
high. A `cyw43` deinit/reinit would reset it; upstream does not do that
during an attempt, but re-check on each release.

**Test.** A `final` build, then listen for whine at idle and during speaker
audio, before and after. Check that USB current rises only a few mA.

---

## 7d. Open DS5Dongle PRs reviewed for DS5_Bridge (2026-11-18)

Reviewed: DS5Dongle PR 301, PR 281 and PR 241. All three were **open, not
merged** on the review date. Write them only in this plain form (never with a
`#` or a URL; see `CONTRIBUTING-FORK.md` §1).

| DS5Dongle PR | What it fixes | Already in DS5_Bridge 2.0.1? | Verdict |
|---|---|---|---|
| **PR 301**: defer USB re-enumeration until host wake/resume settles | On PS-button wake, feature report `0x20` triggers `usb_reconnect()` while the host is still suspended or resuming. The dongle stays stuck and the lightbar stays yellow. | **Mostly.** `usb.cpp`'s reattach logic holds every topology change while `usb_host_suspended` ("host suspended" hold) and while Wake-on-LAN is armed against a sleeping host. The full DS/DSE topology waits for HID init (`usb_handle_controller_transport_ready`). **Not present:** a settle window after `tud_resume_cb()` (PR 301 uses 2 s), and its longer disconnect pulse (300 ms; ours is `USB_RECONNECT_HOLD_US` 100 ms). | **Watch, don't port.** Port the post-resume settle and the 300 ms pulse only if a wake from sleep ends with the controller connected but not enumerated. Hook point if needed: `tud_resume_cb()` records a timestamp; the reattach function gets one more hold next to "host suspended". |
| **PR 281** item 1: keep `NoiseCancelEnable` when the host sets AudioControl | Linux `hid-playstation` sends AudioControl on every headset plug or unplug. That clears the bit and the speaker stays quieter until the controller reconnects. | **Unknown, probably affected on Linux.** We pass complete host `0x02` reports through (`OutputReasonHostPassthrough`). Our own `send_speaker_output_state()` never sets the NoiseCancel bit either. | **Low priority.** This is a Linux-host issue; this fork runs on Windows. Look at it only if a Linux user reports quiet audio after a headset plug. Check first whether DS5Dongle's earlier "force NoiseCancel" fix for the bridge's own reports also changes loudness here. |
| PR 281 item 2: scale headphone volume to 0..127 | DS5Dongle sends the USB volume as `100 + dB` to both amps, so the headset tops out at 100/127. | **Not applicable.** We always send `DS_OUTPUT_HEADPHONE_VOLUME_MAX` (0x7f) and apply volume elsewhere. | Skip. |
| PR 281 item 3: raise the minimum `audio_buffer_length` to 32 | Speaker garbles below ~28 with two-frame packing (DS5Dongle issue 252). | **Same root cause measured here** (`agents/PLAN-audio-stutter.md`: 32 is the floor, 48 is comfortable). Our `MIN_HAPTICS_BUFFER_LENGTH` is still 16. | **Optional small PR.** Raise the firmware minimum to 32 (`src/audio.cpp`) and the companion slider minimum to match. This is a firmware + companion change. Do it only if upstream agrees; it trades a little latency for users who set 16-31. |
| **PR 241**: stabilize Waveshare RP2350B-Plus-W (low-flash layout, USB stays mounted, HID output out of callbacks, `CAN_SEND_NOW` serialization, 47-byte output parse) | On real Waveshare boards, flash near the end of the 16 MiB stalls config/TLV load, so there is no pairing or the board enumerates as `VID_0000&PID_0002`. | **Partly.** DS5_Bridge 2.0 already has the USB, queue and `CAN_SEND_NOW` parts in its own design, and accepts 47-byte output (`DS_OUTPUT_REPORT_COMMON_SIZE` 47). **Not present:** the low-flash bank. We use the SDK default bank at the top of 16 MiB, the exact region PR 241 reports as bad. | **Keep as a known fix, don't port pre-emptively** (as `agents/TASK.md` already says). We have never seen the stall on our board. If it appears: `-DPICO_FLASH_BANK_STORAGE_OFFSET=0x003FD000`. Upstream's WOL store (`kOffset = BANK - 3 sectors`) and the bootloader-authorization sector (`BANK - 1 sector`) are both relative to that define, so they move with it. Cost: pairings, Wake PC credentials and settings in the old bank are lost once (no migration). The firmware (~650 KB) ends well below the new offset. |

---

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
   git grep -nE "host_starting_|magic_frame|saved_lightbar_red" <tag> -- src   # 7a/7b hook points
   ```
4. Command IDs: if any custom command still exists, list every hex value in
   `companion.cpp` `CommandId` and `protocol.ts` `COMMAND_ID`, take the max,
   renumber ours above it in both files together, and bump `kProtocolMinor` /
   `PROTOCOL_MINOR` consistently (and the string assertion in
   `usb_descriptor_migration_test.cpp`). Currently we have **no custom
   commands**.
5. Re-apply features in this order: firmware one-liners and WOL patches (§5, §7, §7b, §7a), then
   companion (§4, §6), then tooling (§8).
6. Check the SDK pin, run host tests, typecheck, vitest, build both sides.
7. Flash and smoke test (WOL, jack switch, idle disconnect), then update §2
   and add a §10 entry.

---

## 10. Release log

| Release | Date | Outcome |
|---|---|---|
| v2.0.1 | 2026-10-29 | Port in progress on branch `port/v2.0.1` (off the tag). Done: WOL switched to upstream's (ours not carried); audio-aware idle disconnect (`bt.cpp` + test); regulator reset; Auto Switch Audio on Jack (AudioHelper, service, IPC, new settings row, all 8 locale catalogs); display-aware idle disconnect; fork tooling (`build-firmware.ps1` without `smoke`, `package:win:local`). Host firmware tests, companion typecheck and 865 vitest tests pass; firmware `final` builds. Pending: hardware smoke test (WOL, jack switch, idle), FIFO re-test (§7), lightbar decision (§7a). |
| v2.0.1 | 2026-10-31 | Documented connect-time fixes and AudioHelper rapid respawn failure mode (churning conhost.exe every 2 s when targeting missing endpoints like RDP 'Remote Audio' on local console; added immediate workaround, config fix, and code backoff guidelines to §4). |
| v2.0.1 | 2026-11-07 | Added two LOCAL-ONLY WOL patches on `port/v2.0.1`: UDP-broadcast magic frame (§7b) after upstream's raw-frame WOL did not wake the PC, and the green lightbar indicator (§7a). Host tests pass; `final` and `debug` build and pass the link-time checks. Pending: hardware smoke test of both (does the PC wake, does the light flash/solid/restore). |
| v2.0.1 | 2026-11-18 | Compared with DS5Dongle v0.8.0. Two fixes built as separate unpublished PR branches off `v2.0.1`: `pr/cyw43-init-reboot` (`c8cd697`, §7c.1) and `pr/waveshare-smps-pwm` (`ea0b050`, §7c.2, with the Waveshare board-header define). Both are also applied (uncommitted) on `port/v2.0.1`. `final` builds, host tests pass, hardware test pending. Reviewed DS5Dongle PRs 301, 281 and 241 (§7d): none ported now; PR 281's buffer-minimum-32 is an optional small PR; PR 241's low-flash bank stays the documented fallback. |

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
6. Does Waveshare's schematic confirm `WL_GPIO1` is the regulator mode pin?
   (§7c.2. Decides whether the board-header define goes into that PR.)
7. Raise the audio buffer minimum to 32 as an upstream PR, or leave it as
   guidance? (§7d, PR 281 item 3.)
