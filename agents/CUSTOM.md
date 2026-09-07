# CUSTOM.md — this fork's custom features, and how to re-apply them

This fork carries **two additive product changes** over upstream
`SundayMoments/DS5_Bridge`:

1. **Wake-on-LAN over Wi-Fi** (firmware + companion) — when a DualSense
   controller connects, the firmware brings up the CYW43 radio's Wi-Fi side
   and sends a UDP magic packet to a configured target PC. Upstreamed as
   **DS5_Bridge PR 120**.
   Sections **1–9** below.
2. **Auto Switch Audio on Jack** (companion only, **no firmware change**) —
   when a headset is plugged into the controller's 3.5 mm jack, make the
   controller the Windows default audio output; on unplug, switch back to a
   chosen (or auto-detected) fallback device. Section **10** below.
   PR not yet opened (pending a 3+ output-device UI test — see `TASK.md`).

3. **Audio-aware Idle Disconnect** (firmware only) — the existing Idle
   Disconnect only checks for absent HID input; this makes it also skip
   disconnecting while audio is actively routed to the controller (a movie or
   music through the headset jack). One `if` in `src/bt.cpp`. Section **11**
   below. Separate small firmware PR (not yet opened).

This file exists so that **if a PR is never merged**, each feature can be
re-applied onto any future DS5_Bridge release by hand, by touching a known,
short list of files at known points.

Read alongside:

- **AGENTS.md** — the "keep it additive / rebase-friendly" working rules and
  the rebase/backport checklist.
- **DECISIONS.md** — *why* each hook point and wire-format choice is what it
  is. This file says *where*; that file says *why*.
- **PLAN-headset-audio-switch.md** — the full design history + smoke-test
  notes for feature #2.
- **src/wolwifi.h** — feature #1's module contract in one screen.
No audio debug events captured yet.
---

## 1. Design in one paragraph

Everything that is *Wi-Fi, magic packet, retry, host-alive gate, and flash
persistence* lives in **two new self-contained files** (`src/wolwifi.cpp`,
`src/wolwifi.h`). On a board without the feature compiled in, `wolwifi.h`
supplies inline **no-op stubs** for every entry point, so callers never need
an `#ifdef`. Everything else in the diff is either (a) a **one-line call** into
that module from an existing file, (b) a **new enum value / struct field**
appended to an existing list, or (c) **new support files** that don't modify
anything. There is deliberately no restructuring of upstream code anywhere.

So a port is: **drop in the new files, re-add ~12 small hook points, renumber
the 4 command IDs if they now collide, rebuild both sides.**

---

## 2. The complete file inventory

### 2a. New files — copy verbatim, they modify nothing

| File | Purpose | Notes on porting |
|---|---|---|
| `src/wolwifi.h` | Module contract + no-op stubs | Copy as-is. |
| `src/wolwifi.cpp` | The whole feature: Wi-Fi connect state machine, DHCP wait, magic-packet send, ARP-liveness resend cycle, `ObserveHost` host-alive gate, flash (BTstack TLV, tag `'WOLC'`) persistence | Copy as-is. Only depends on: `pico/cyw43_arch.h`, lwIP raw API (`udp`/`dhcp`/`etharp`/`netif`), `btstack_tlv.h`, and this repo's `bt.h` / `usb.h` / `utils.h`. If upstream renames a function `wolwifi.cpp` calls (see §3), fix the call. |
| `boards/headers/lwipopts.h` | lwIP config: `NO_SYS=1`, IPv4/UDP/DHCP/ARP only, tiny heap, DHCP ARP-check disabled for BT/Wi-Fi radio-coexistence | Copy as-is. Only consumed when `ENABLE_WOLWIFI` is on (the `CMakeLists.txt` block adds `boards/headers` to the include path only then). |
| `tools/build-firmware.ps1` | This fork's one firmware build path (Waveshare, versioned/variant UF2 staging) | Dev convenience, not part of the feature. Port only if you want this fork's build workflow. |
| `tools/rebuild-companion.ps1` | Kill → `package:win:local` → relaunch the local companion build | Same — dev convenience. |
| `boards/run_firmware_tests.sh` | Host-side firmware test runner into `build/waveshare-tests/` | Same — dev convenience. |
| `AGENTS.md`, `DECISIONS.md`, `CHANGELOG.md`, `TASK.md`, this file | Fork tracking docs | Not shipped upstream; keep in the fork. |

### 2b. Modified files — all changes are additive hook points

Firmware:

| File | What was added | Kind |
|---|---|---|
| `src/bt.cpp` | `#include "wolwifi.h"`; one call `wolwifi_on_controller_connect();` in `finish_hid_session_if_ready()`; the `bt_wol_indicator_*()` lightbar-pulse helpers (self-contained block near `bt_lightbar_loop()`) | 1 call site + 1 new block |
| `src/bt.h` | Declarations for the 4 `bt_wol_indicator_*()` functions | new decls |
| `src/main.cpp` | `#include "wolwifi.h"`; `wolwifi_init();` after `bt_init()`; `wolwifi_task();` as a new `RUN_MAIN_PHASE`; `bt_wol_indicator_loop();` next to `bt_lightbar_loop();` in the Lightbar phase | 3 call sites |
| `src/usb.cpp` | `#include "wolwifi.h"`; new `usb_host_active()`; in `usb_pm_poll()` wrap the existing power-off in `if (!wolwifi_wake_in_progress()) { ... }` | 1 new fn + 1 guard |
| `src/usb.h` | `bool usb_host_active();` declaration | new decl |
| `src/watchdog_telemetry.h` | `Wolwifi = 16` appended to `WatchdogMainLoopPhase` | new enum value |
| `src/watchdog_telemetry.cpp` | `case WatchdogMainLoopPhase::Wolwifi: return "wolwifi";` | new switch case |
| `src/companion.cpp` | `#include "wolwifi.h"`; 4 new `CommandId` enum values (`0x46`–`0x49`); 4 new `case` blocks in `handle_command()` | new enum values + new cases |
| `CMakeLists.txt` | `option(ENABLE_WOLWIFI ...)` + `option(WOL_ALWAYS ...)`; conditional `target_sources(... src/wolwifi.cpp)`; a `if (ENABLE_WOLWIFI) ... else ()` block that swaps `pico_cyw43_arch_poll` → `pico_cyw43_arch_lwip_poll`, adds the `ENABLE_WOLWIFI` define, `boards/headers` include, and moves the `CYW43_LWIP=0` define into the `else` branch | see §4 |

Companion app (all in `companion/`):

| File | What was added | Kind |
|---|---|---|
| `src/shared/protocol.ts` | 4 `COMMAND_ID` entries (`0x46`–`0x49`); `wolControl` in `firmwareFlags` + its `report[6] >= 20` gate in `parseStatusReport`; `wolTargetMacPayload()`, `wolWifiSsidPayload()`, `wolWifiPasswordPayload()`, `WOL_WIFI_SSID_MAX_LENGTH`, `WOL_WIFI_PASSWORD_MAX_LENGTH` | new consts + fns |
| `src/shared/types.ts` | `wolEnabled`, `wolWifiSsid`, `wolWifiPassword`, `wolTargetMac` on `CompanionSettings` | new fields |
| `src/main/settings-store.ts` | Those 4 in `DEFAULT_SETTINGS` and `normalizeSettings()` | new fields |
| `src/main/bridge-service.ts` | `setWolEnabled/Ssid/Password/TargetMac()` methods; 4 sends appended to the post-connect full-reapply sequence | new methods + reapply lines |
| `src/main/main.ts` | 4 `ipcMain.handle('bridge:setWol*')` registrations | new IPC |
| `src/preload.ts` | 4 `setWol*` bridge API methods | new IPC |
| `src/renderer/App.tsx` | `wolSupported` flag; the "Wake-on-LAN" settings section (toggle + SSID/password/MAC inputs with inline validation); 4 draft-state `useState` hooks. **Note:** the diff also *moves* the "Power & Controller" / "Firmware" column split — that move is cosmetic and the most likely merge-conflict spot; re-do it by hand or drop it. | new UI section (+ a cosmetic reflow) |
| `src/renderer/styles.css` | `.wol-settings-row/-input/-error` rules; a wider `.bridge-settings-preferences-modal` | new CSS |
| `src/main/bridge-service.test.ts` | `SET_WOL_ENABLED` in `FULL_REAPPLY_COMMANDS`; the "sends the SSID/password byte length" regression test | new test |
| `src/main/ipc-contract.test.ts` | "exposes the Wake-on-LAN over Wi-Fi preferences" test | new test |
| `package.json` | `package:win:local` script | fork convenience |
| `scripts/package-win.mjs` | optional out-dir override arg | fork convenience |
| `README.md` | Wake-on-LAN user docs + path-map row | docs |

---

## 3. Firmware hook points — exact locations

### H1. `src/bt.cpp` — the controller-connect trigger  ⭐ the one essential hook

In `finish_hid_session_if_ready()`, right after `connection_phase` is set to
`BtConnectionPhase::Ready`:

```cpp
connection_phase = BtConnectionPhase::Ready;
connection_phase_started_us = 0;
cancel_hid_channel_recovery_if_ready();
wolwifi_on_controller_connect();   // <-- ADD
```

Why here: this function early-returns if `connection_phase` is already
`Ready`, so this line runs **exactly once per DISCONNECTED→CONNECTED edge** —
natural edge-triggering, no debounce needed. (Upstream PR #93's
`wake_on_bt_connect()` used the same call site; #93 is reference-only, do not
cherry-pick it.) Also add `#include "wolwifi.h"` at the top.

If upstream renames or restructures `finish_hid_session_if_ready()`: the
requirement is simply *"call `wolwifi_on_controller_connect()` once, on the
edge where a BT controller connection becomes fully ready for input."* Find
that edge and call it there.

### H2. `src/bt.cpp` / `bt.h` — the lightbar WOL indicator

A self-contained block: `WolIndicatorPhase` enum + `wol_indicator_*` statics
+ `bt_wol_indicator_begin/confirm/cancel/loop()`, added near
`bt_lightbar_loop()`. It needs its **own** color snapshot
(`wol_indicator_pre_*`) because `bt_set_lightbar_color()` clobbers the normal
restore target every call (see DECISIONS.md "lightbar WOL indicator needs its
own color snapshot"). `wolwifi.cpp` calls `bt_wol_indicator_begin/confirm/
cancel()`; `main.cpp` calls `bt_wol_indicator_loop()`. Purely cosmetic — if
the port is time-boxed, these 4 can be stubbed empty and WOL still works.

### H3. `src/main.cpp` — init + task pump

```cpp
bt_init();
bt_register_data_callback(on_bt_data);
wolwifi_init();                       // <-- ADD (after cyw43_arch_init, which bt_init triggers)

watchdog_enable(1000, true);
```

```cpp
RUN_MAIN_PHASE(WatchdogMainLoopPhase::UsbPower, { usb_pm_poll(); });
RUN_MAIN_PHASE(WatchdogMainLoopPhase::Wolwifi,  { wolwifi_task(); });   // <-- ADD
```

```cpp
RUN_MAIN_PHASE(WatchdogMainLoopPhase::Lightbar, {
    bt_lightbar_loop();
    bt_wol_indicator_loop();          // <-- ADD
});
```

Plus `#include "wolwifi.h"`. `wolwifi_init()` must come *after* the CYW43
radio is initialised (BTstack's `bt_init()` does that) and *before* the main
loop. `wolwifi_task()` must be polled every iteration — it's the state-machine
pump and never blocks.

### H4. `src/main/watchdog_telemetry.{h,cpp}` — phase name

`Wolwifi = 16` on the `WatchdogMainLoopPhase` enum (next free value) and the
matching `case ... return "wolwifi";`. Required only because H3 adds a new
`RUN_MAIN_PHASE`; pick whatever the next free enum value is on the target
release.

### H5. `src/usb.cpp` / `usb.h` — host-alive gate signal + power-off deferral

New function (the real production signal the host-alive gate samples):

```cpp
bool usb_host_active() {
    return usb_mounted && !usb_bus_suspended();
}
```

In `usb_pm_poll()`, the existing suspend-driven power-off becomes:

```cpp
if (!wolwifi_wake_in_progress()) {          // <-- WRAP the existing 2 lines
    (void)bt_power_off_controller();
    usb_suspend_at_us = 0;
}
```

Why: without this, the board powers the controller off ~3s after the host
suspends — cutting a wake attempt short before it can land. Leaving
`usb_suspend_at_us` set means it re-checks every tick and fires the instant
the wake finishes. Plus `#include "wolwifi.h"` and the `usb.h` declaration.
If upstream's suspend/power-off logic moved, the rule is: *don't power the
controller off while `wolwifi_wake_in_progress()` is true; retry as soon as
it clears.*

### H6. `src/companion.cpp` — 4 setting commands

`#include "wolwifi.h"`, 4 new `CommandId` values, 4 new `case` blocks in
`handle_command()`. See §5 for the collision check on the ID numbers, and §6
for the wire format the `case` blocks parse.

---

## 4. `CMakeLists.txt` — the build wiring

Two `option()`s near the other board options:

```cmake
option(ENABLE_WOLWIFI "Enable Wake-on-LAN over Wi-Fi (requires CYW43 Wi-Fi + lwIP)" ${WAVESHARE_RP2350B_PLUS_W_BUILD})
option(WOL_ALWAYS      "Skip the host-alive gate and always send WOL on a debounced controller connect" OFF)
```

Conditional source:

```cmake
if (ENABLE_WOLWIFI)
    target_sources(ds5-bridge PRIVATE src/wolwifi.cpp)
endif ()
```

The link/define swap — upstream links `pico_cyw43_arch_poll` and defines
`CYW43_LWIP=0` unconditionally; this fork makes that the `else` branch:

```cmake
if (ENABLE_WOLWIFI)
    # lwIP on top of the same poll-mode CYW43 arch BTstack already uses.
    target_include_directories(ds5-bridge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/boards/headers)
    target_compile_definitions(ds5-bridge PRIVATE ENABLE_WOLWIFI)
    if (WOL_ALWAYS)
        target_compile_definitions(ds5-bridge PRIVATE WOL_ALWAYS)
    endif ()
    target_link_libraries(ds5-bridge pico_cyw43_arch_lwip_poll)
else ()
    target_compile_definitions(ds5-bridge PRIVATE CYW43_LWIP=0)
    target_link_libraries(ds5-bridge pico_cyw43_arch_poll)
endif ()
```

On the target release: find where upstream links `pico_cyw43_arch_poll` and
where it defines `CYW43_LWIP=0`, and move **both** into a new `else ()` of an
`if (ENABLE_WOLWIFI)`. `pico_cyw43_arch_lwip_poll` pulls in `pico_lwip_nosys`
and sets `CYW43_LWIP=1` via its headers interface; BTstack keeps using the
CYW43 HCI transport regardless. `boards/headers/lwipopts.h` is picked up via
the include dir added above.

---

## 5. Command-ID collision check — do this every port ⚠️

This fork's 4 WOL command IDs are **`0x46`–`0x49`** in both
`src/companion.cpp` (`CommandId` enum) and
`companion/src/shared/protocol.ts` (`COMMAND_ID`). They have already been
renumbered once (from `0x37`–`0x3A`) after upstream v1.7.0 landed
`SET_RADIAL_DEADZONES` at `0x37` — a silent collision `git merge` cannot
detect (see DECISIONS.md "merging/rebasing … can collide COMMAND_ID values").

**Every time you port onto a newer release:**

1. `grep -n "0x[0-9A-Fa-f]\+" ` both enums (`CommandId` in `companion.cpp`,
   `COMMAND_ID` in `protocol.ts`).
2. Take the **max assigned value across both**.
3. If any of `0x46`–`0x49` is now used by upstream, renumber this fork's 4 IDs
   to `max+1 .. max+4`, **in both files together** (they must always match).
4. Bump `PROTOCOL_MINOR` / `kProtocolMinor` in both files, combining
   upstream's bump with this fork's. Update the hardcoded
   `constexpr uint8_t kProtocolMinor = N;` string assertion in
   `tests/firmware/usb_descriptor_migration_test.cpp` — it fails loudly on a
   mismatch, which is the point, but still needs the new value by hand.
5. Update the `wolControl` gate `report[6] >= 20` in `protocol.ts` only if the
   minor value it should require actually changes (it gates *feature
   presence*, not the latest minor — leave it at the minor where WOL first
   shipped unless you have a reason).

---

## 6. Wire format (so the `companion.cpp` cases and `protocol.ts` payloads agree)

| Command | `value` field | Trailing payload | Firmware entry point |
|---|---|---|---|
| `SET_WOL_ENABLED` (`0x46`) | `0` / `1` | — | `wolwifi_set_enabled()` |
| `SET_WOL_WIFI_SSID` (`0x47`) | byte length | SSID bytes at `buffer + 10` | `wolwifi_set_wifi_ssid(ptr, len)` |
| `SET_WOL_WIFI_PASSWORD` (`0x48`) | byte length | password bytes at `buffer + 10` | `wolwifi_set_wifi_password(ptr, len)` |
| `SET_WOL_TARGET_MAC` (`0x49`) | `0` (unused) | 6 raw MAC bytes at `buffer + 10` | `wolwifi_set_target_mac(ptr)` |

- SSID/password reuse **`SET_CHORD_BINDINGS`'s variable-length framing**
  (length in `value`, bytes after the 10-byte header). The HID report is
  fixed 63 bytes, so the payload budget is `COMPANION_PAYLOAD_SIZE - 10`.
  `WOL_WIFI_PASSWORD_MAX_LENGTH = 53` (< WPA2's 63) and
  `WOL_WIFI_SSID_MAX_LENGTH = 32` (802.11 limit).
- MAC reuses the **6-byte Bluetooth-address wire format**, not length-prefixed
  (a MAC is always 6 bytes). `wolTargetMacPayload()` additionally rejects
  all-zero and all-`0xFF`.
- Firmware **re-validates every length** — it never trusts the companion's
  client-side checks. Setters return `false` and keep the prior value on a
  bad length / null pointer, and the `case` block ACKs `AckInvalidValue`.
- **Known past bug (there's a regression test for it):** the companion setters
  must put the **payload length in `value`**, not `0` — firmware reads `value`
  as the byte count to copy, so a `0` there silently stored an empty SSID. See
  `bridge-service.test.ts` "sends the SSID/password byte length".

---

## 7. Flash persistence (no hook — just don't break it)

`wolwifi.cpp` persists config to on-board flash via **BTstack's TLV store**,
tag `'WOLC'` (`0x574F4C43`) — same mechanism as pairing-key persistence. It
loads in `wolwifi_init()`, before BT can complete a reconnect, so a paired
controller reconnecting fast doesn't fire the trigger with an empty RAM
config. This is entirely inside `wolwifi.cpp`; a port only needs BTstack's TLV
API (`btstack_tlv_get_instance`) to still exist, which it will. See
DECISIONS.md "WOL config persists to on-board flash".

---

## 8. Port procedure, start to finish

1. **Branch** off the new upstream tag (this fork uses merge, not rebase —
   see AGENTS.md).
2. **Copy new files** (§2a): `src/wolwifi.{h,cpp}`, `boards/headers/lwipopts.h`.
   (The `tools/*.ps1` and `boards/run_firmware_tests.sh` only if you want the
   fork's build workflow.)
3. **Firmware hooks** (§3): H1 (essential), H3, H4, H5, H6, then H2 (cosmetic
   — stub if short on time).
4. **`CMakeLists.txt`** (§4): the two options + the `if/else` link-and-define
   swap.
5. **Command-ID collision check** (§5) — do not skip even if the merge
   reported no conflict.
6. **Companion app**: `types.ts` fields → `settings-store.ts` defaults +
   normalize → `protocol.ts` IDs/payloads/`wolControl` → `bridge-service.ts`
   methods + reapply → `main.ts` + `preload.ts` IPC → `App.tsx` UI section →
   `styles.css`. Re-do the `App.tsx` column reflow by hand or drop it.
7. **Build & test both sides**:
   - Firmware: `.\tools\build-firmware.ps1` (or a plain
     `-DWAVESHARE_RP2350B_PLUS_W_BUILD=ON` CMake build) + `./boards/run_firmware_tests.sh`.
   - Companion: `cd companion && npm run typecheck && npx vitest run src`.
     The two new tests (`ipc-contract` WOL test, `bridge-service` byte-length
     test) plus `FULL_REAPPLY_COMMANDS` must pass.
8. **Hardware smoke test**: configure SSID/password/MAC in the app, shut the
   PC down, connect the controller, confirm it wakes. Then confirm it *doesn't*
   send when the PC is already on (host-alive gate). Use the `debug` firmware
   variant for `[WOL]` UART logs if something's off (see AGENTS.md
   Diagnostics).

---

## 9. What is safe to drop if you're time-boxed

- **The lightbar indicator (H2)** — cosmetic. Stub `bt_wol_indicator_*()`
  empty.
- **`WOL_ALWAYS` option** — only needed for motherboards that keep USB active
  in S5 / Modern Standby. Omit until someone needs it.
- **The `App.tsx` column reflow** — keep upstream's column layout, just add
  the WOL section into whichever column has room.
- **`tools/*.ps1`, `run_firmware_tests.sh`, `package:win:local`** — fork
  build conveniences, not the feature.

**What you cannot drop:** the new files, H1, H3, H5, H6, the `CMakeLists.txt`
link swap, the command-ID check, and the companion protocol/settings/IPC
plumbing. That's the irreducible feature.

---

# Feature #2 — Auto Switch Audio on Jack

## 10. Overview

**Companion-app only. No firmware change, no protocol change, no
`COMMAND_ID`, no `PROTOCOL_MINOR` bump.** It consumes a bit the firmware
already sends (`headsetPlugged`, see §10.2) and drives the Windows default
audio endpoint through the companion's existing `AudioHelper` native process.

Behavior:

- Headset **in** the controller's 3.5 mm jack → Windows default *render*
  endpoint = the controller/bridge endpoint.
- Jack **empty** → default render endpoint = a **fallback device**: either the
  one the user picked in a dropdown, or — when exactly one non-controller
  render endpoint exists — that one automatically.
- Also corrects the case where Windows re-promotes the controller to default
  while the jack is empty.
- Debounced (2 consecutive ~500 ms audio-status polls) because the firmware
  bit is a raw pass-through with no debouncing of its own.
- Dormant during an RDP session (Windows redirects audio to "Remote Audio";
  don't fight it) and during host-persona transitions / render-restores.
- One toggle (`headsetAudioAutoSwitchEnabled`) + one optional fallback-device
  string (`headsetAudioFallbackDevice`, `''` = auto). Both default off/empty.

## 10.1. Complete file list (all under `companion/`)

Every change is additive — a new method, a new setting field, a new IPC pair,
a new UI row, a new CSS block, a new AudioHelper verb. Nothing upstream is
restructured.

| File | What was added |
|---|---|
| `native/AudioHelper/EndpointManager.cs` | `SetDefaultRenderEndpointByName(string candidates)` — set default render to the first *active* endpoint whose FriendlyName matches one of `;`-separated candidates; throws (non-zero exit) if none. `ListRenderEndpoints()` — print active render endpoints as JSON `[{name,isBridge}]` on stdout (`isBridge` via existing `IsKnownBridgeEndpoint`). |
| `native/AudioHelper/Program.cs` | Two new CLI flags: `--set-default-render` (uses the existing `--device-name` arg) and `--list-render-endpoints`. New `SetDefaultRender` / `ListRenderEndpoints` bools on the `HelperOptions` record + their arg-parse cases + dispatch in `Main`. |
| `src/main/audio-helper.ts` | `setDefaultRenderEndpointByName(names[])` and `listRenderEndpoints(): Promise<RenderEndpointInfo[]>` wrappers (spawn the helper via the existing `runAudioHelperCommand`). New exported `RenderEndpointInfo` type. |
| `src/shared/types.ts` | `headsetAudioAutoSwitchEnabled: boolean` and `headsetAudioFallbackDevice: string` on `CompanionSettings`. |
| `src/main/settings-store.ts` | Both in `DEFAULT_SETTINGS` (`false` / `''`) and in `normalizeSettings()`. **Not** in `CONTROLLER_PROFILE_SETTING_KEYS` — these are global, not per-profile. |
| `src/main/bridge-service.ts` | The feature block (see §10.3): `syncHeadsetAudioAutoSwitch()` called each poll; `resolveHeadsetAudioFallback()`; `correctDefaultRenderIfBridgeWhileJackEmpty()`; `isRemoteSessionActive()`; `listRenderEndpointNames()`; `setHeadsetAudioAutoSwitchEnabled()` / `setHeadsetAudioFallbackDevice()` / `reevaluateHeadsetAudioAutoSwitch()`. New instance state: `headsetJack{ActedState,Pending,PendingCount}`, `headsetAudioSwitchInFlight`, `cachedRenderEndpoints`. Instance-method wrappers `this.setDefaultRenderBridgeEndpoint` / `this.setDefaultRenderEndpointByName` / `this.getDefaultRenderEndpointStatus` (so tests can inject). Reset the jack state in `closeDevice()`. |
| `src/main/main.ts` | `bridge:setHeadsetAudioAutoSwitchEnabled`, `bridge:setHeadsetAudioFallbackDevice`, `bridge:listRenderEndpointNames` IPC handlers. |
| `src/preload.ts` | The three matching `window.bridge.*` methods. |
| `src/renderer/App.tsx` | "Auto Switch Audio on Jack" row in Bridge Settings > Power & Controller: a toggle, plus a `CustomSelect` fallback dropdown shown *only* when there are ≥2 non-controller outputs (or a saved-but-absent device). `renderEndpoints` state + a `useEffect` that calls `listRenderEndpointNames()` whenever Bridge Settings opens; `headsetAudioOutputChoices` / `headsetAudioShowFallbackSelect` / `headsetAudioFallbackOptions` derived values. |
| `src/renderer/styles.css` | `.headset-audio-controls` (flex group, flush right) + `.headset-audio-fallback-select` sizing. The **full-window** `.bridge-settings-preferences-modal` (`100vw`/`100vh`, no border/radius) + `.bridge-settings-backdrop { padding: 0 }`. **One LOCAL-ONLY block** (clearly commented) neutralising `:disabled` dimming inside this modal — see §10.5, drop it for the PR. |
| `src/main/bridge-service.test.ts` | 6 tests under `describe('Auto Switch Audio on Jack')` + the shared `createService` fixture baselines `headsetAudioAutoSwitchEnabled: false`. |
| `src/main/ipc-contract.test.ts` | One test covering the three new IPC channels + service method signatures. |

## 10.2. The signal — already in upstream, no firmware work

`companion/src/shared/protocol.ts` already parses, from the audio status
report's route-flags byte:

```ts
headsetPlugged: (routeFlags & 0x01) !== 0,   // report[10] bit 0
```

Traced to source: firmware `main.cpp` calls `set_headset((controller_report[53]
& 1) != 0)` on every DualSense interrupt-in report — a **raw, unfiltered
pass-through of input-report byte 53 bit 0** (`PluggedHeadphones`), no
debounce anywhere. Bit 1 (`headsetAudioRoute`) is firmware-internal routing
state (false whenever nothing is playing) — **do not use it**. Gate on bit 0.

In `bridge-service.ts` it's `this.audioStatus.headsetPlugged`, refreshed every
~500 ms by the existing `readAudioStatus()`.

## 10.3. The hook point — one call in the poll loop

`bridge-service.ts`, `poll()`, right after the existing
`syncControllerPowerSavingState(settings)`:

```ts
await this.syncControllerPowerSavingState(settings);
await this.syncHeadsetAudioAutoSwitch(settings);   // <-- ADD
```

`syncHeadsetAudioAutoSwitch()`:

1. Early-return unless `settings.headsetAudioAutoSwitchEnabled` (clears the
   debounce state).
2. Early-return if not `connected`, no `audioStatus`, a switch is in flight,
   RDP is active, a host-persona transition / `hostPersonaDefaultRenderRestore`
   is in flight.
3. If the toggle is on but no explicit fallback is set and
   `cachedRenderEndpoints` is empty, refresh it once (`await
   listRenderEndpoints()`).
4. `resolveHeadsetAudioFallback()` → explicit `headsetAudioFallbackDevice`, or
   the sole non-`isBridge` endpoint name, or `''`. If `''`, stay idle.
5. Debounce: track `headsetJackPending`/`PendingCount`; a *changed*
   `headsetPlugged` must hold `HEADSET_AUDIO_JACK_DEBOUNCE_POLLS` (=2) polls.
   The first evaluation (`headsetJackActedState === null`) is applied
   immediately.
6. If `headsetJackActedState === jackPlugged`: already on target; if the jack
   is empty, run `correctDefaultRenderIfBridgeWhileJackEmpty(fallback)`
   (queries the current default; re-routes only if it's the bridge endpoint).
7. Otherwise set `headsetJackActedState`, then:
   - jack plugged → `this.setDefaultRenderBridgeEndpoint(settings.hostPersonaMode)`
   - jack empty → `this.setDefaultRenderEndpointByName([fallback])`
   All fire-and-forget with try/catch; on error clear `headsetJackActedState`
   so the next poll retries. `headsetAudioSwitchInFlight` guards re-entry.

Reset `headsetJack{ActedState,Pending,PendingCount}` in `closeDevice()` next
to the existing `controllerPowerSavingActive = null`.

## 10.4. AudioHelper verbs

Both live next to the existing `--set-default-render-bridge` /
`--default-render-status`:

- **`--set-default-render --device-name "A;B;C"`** → first *active* render
  endpoint whose FriendlyName equals or contains a candidate; sets it default
  for Console + Multimedia roles (reuses the private
  `SetDefaultRenderEndpoint`). Non-zero exit if none — the TS wrapper's retry
  sees it. Used only for the "jack empty" direction (the "jack plugged"
  direction keeps using `--set-default-render-bridge`).
- **`--list-render-endpoints`** → `[{ "name": "...", "isBridge": true|false }]`
  on stdout, ordered by name. `isBridge` via the existing
  `IsKnownBridgeEndpoint` (container-id + name-alias match) so the companion
  can exclude the controller's own endpoint from the dropdown and from the
  auto-resolve.

## 10.5. What to change before opening the PR

- **Drop the LOCAL-ONLY CSS block** in `styles.css` (the
  `.bridge-settings-preferences-modal ... :disabled { opacity: 1 }` rule,
  clearly commented). It hides a pre-existing, harmless full-window dim while
  a setting write is in flight — kept locally only because the full-window
  modal makes it obvious. Not a change upstream needs bundled with this
  feature.
- **The full-window modal** (`.bridge-settings-preferences-modal` at
  `100vw`/`100vh`, `.bridge-settings-backdrop { padding: 0 }`) is a local
  preference. Decide whether it belongs in the PR or should be reverted to
  the upstream centred panel — it's orthogonal to the audio feature.
- Everything else (the AudioHelper verbs, the setting, the IPC, the poll
  hook, the UI row, the tests) is the feature and ships as-is.
- Confirm no `COMMAND_ID` / `PROTOCOL_MINOR` touch — there should be none.
- PR framing: answers upstream issue-style request #254; the maintainer
  already said it needs "a small Windows application" — the companion *is*
  that application and had ~90% of the plumbing (`IPolicyConfig` switching,
  the render-restore state-machine shape).

## 10.6. Porting onto a newer upstream release

1. Copy the two AudioHelper methods + the two `Program.cs` flags/fields.
2. `audio-helper.ts`: the two wrappers + `RenderEndpointInfo`.
3. `types.ts` / `settings-store.ts`: the two setting fields (defaults +
   normalize).
4. `bridge-service.ts`: the feature block + the one call in `poll()` after
   `syncControllerPowerSavingState`. If upstream renamed/moved that method,
   the rule is just "once per poll, after the audio status read, with a fresh
   `this.audioStatus`".
5. `main.ts` / `preload.ts`: the three IPC channels.
6. `App.tsx`: the settings row + the `renderEndpoints` state/effect/derived
   values. Slot the row into whichever settings column has space.
7. `styles.css`: `.headset-audio-controls` + `.headset-audio-fallback-select`.
   Re-decide the full-window-modal and LOCAL-ONLY blocks per §10.5.
8. Tests: the `describe('Auto Switch Audio on Jack')` block + the fixture
   baseline + the ipc-contract test.
9. `npm run typecheck` + `npx vitest run src` + `npm run build:audio-helper`
   + `.\tools\rebuild-companion.ps1`, then the smoke test in
   `PLAN-headset-audio-switch.md` §7.

---

# Feature #3 — Audio-aware Idle Disconnect

## 11. Overview

**Firmware only.** No new file, no companion change, no protocol change, no
`COMMAND_ID`, no `PROTOCOL_MINOR` bump, no UI. Idle Disconnect's existing
companion controls (enable toggle + timeout) are unchanged; they just become
correct.

**The problem:** `src/bt.cpp`'s idle-disconnect check resets its inactivity
clock (`inactive_time`) only on meaningful HID *input*. Watching a movie with
a headset in the DualSense's 3.5 mm jack and no button press for the timeout
window (default 15 min) → the controller idle-disconnects and cuts the audio.

**The fix:** treat "audio actively routed to the controller" the same as
input activity. The firmware already has the exact signal
(`audio_output_route_protected()` = `audio_recent() ||
usb_speaker_streaming_active()`), already used by the RSSI idle gate and
output-route protection, already in `bt.cpp`.

## 11.1. The change — one `if` in `src/bt.cpp`

In `l2cap_packet_handler_cold()`, the `channel == hid_interrupt_cid` branch,
the `// Inactivity detection.` block. **After** the existing `if (mute[1])
return;` mic-mute carve-out, **before** the timeout comparison:

```cpp
    // Inactivity detection.
    if (mute[1]) { // Microphone mute is enabled.
        return;
    }
    // Audio actively routed to the controller (a movie or music through the
    // 3.5 mm headset jack) means the device is in use even with no button
    // input -- don't idle-disconnect and cut the audio. Reset the idle clock
    // so the full timeout starts fresh once playback stops, matching how a
    // button press resets it. Same signal the RSSI idle gate and
    // output-route protection use.
    if (audio_output_route_protected()) {          // <-- ADD (4 lines)
        inactive_time = now_us;
        return;
    }
    if (!meaningful_input_activity && now_us - inactive_time > idle_disconnect_timeout_us()) {
        ...
    }
```

Why `inactive_time = now_us` and not a bare `return`: so that when audio
finally stops, the full idle timeout restarts from that moment (same as a
button press). A bare `return` would leave a 2-hour movie's end ~0 s from
tripping the timeout on the next input-less packet.

`audio_output_route_protected()` is forward-declared at `bt.cpp:~376` and
defined at `bt.cpp:~4959`; both `audio_recent()` (`audio.h`) and
`usb_speaker_streaming_active()` (`usb.h`) are already linked. Nothing else
to add.

## 11.2. Test

`tests/firmware/usb_descriptor_migration_test.cpp` (source-text assertions —
it doesn't compile `bt.cpp`). The existing RSSI-idle test block (`extract`s
the region from `const bool meaningful_input_activity =` through the idle
disconnect) was extended to also assert the idle block:

- contains `if (audio_output_route_protected())`,
- with the exact `inactive_time = now_us; return;` body,
- ordered **after** `if (mute[1])` and **before** the
  `now_us - inactive_time > idle_disconnect_timeout_us()` comparison.

Run: `./boards/run_firmware_tests.sh`.

## 11.3. Porting onto a newer upstream release

1. In `src/bt.cpp`, find the idle-disconnect block (search
   `idle_disconnect_timeout_us()` or
   `BtControllerDisconnectIntentIdleTimeout`).
2. Confirm `audio_output_route_protected()` (or an equivalent
   `audio_recent() || usb_speaker_streaming_active()` helper) still exists in
   `bt.cpp` — grep for it; upstream uses it in `bt_signal_strength_loop()`
   too.
3. Add the 4-line `if` after the `mute[1]` carve-out, before the timeout
   comparison.
4. Update the assertion in `usb_descriptor_migration_test.cpp` if the
   surrounding text shifted.
5. `.\tools\build-firmware.ps1` + `./boards/run_firmware_tests.sh`. No
   companion rebuild.

## 11.4. PR framing

Small self-contained firmware fix: "Idle Disconnect ignores audio playback and
disconnects the controller mid-movie when it's being used as a headset." One
`if`, reusing a helper the codebase already trusts for the same "is audio
live" question, mirroring the existing `mute[1]` carve-out (same location,
same shape). No protocol/UI change.
