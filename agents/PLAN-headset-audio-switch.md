# Plan — Feature #2: auto-switch Windows audio to the controller only when a headset is plugged in

Status: **implemented, pending hardware smoke test**. Second custom feature
for this fork, after Wake-on-LAN (see CUSTOM.md). Will be proposed upstream as
a separate PR, tracked against issue-style request #254 ("route audio to the
controller only when a headset is plugged in").

### Redesign #2 after second smoke test (2026-09-06)

Second smoke test passed; UX refined per feedback:

- **Row is now a toggle + (conditional) dropdown**, "Idle Disconnect" pattern,
  right-aligned via `settings-menu-controls`. Title **"Auto Switch Audio on
  Jack"**, description "Plug a headset into the controller and Windows output
  follows it; unplug and it returns to the device below."
- **Two settings**: `headsetAudioAutoSwitchEnabled: boolean` (the toggle) +
  `headsetAudioFallbackDevice: string` (the dropdown; `''` = auto).
- **Dropdown only shown when ambiguous** — 2+ non-controller outputs (or a
  saved device that's currently absent). With exactly one other output the
  feature auto-uses it (`resolveHeadsetAudioFallback()` → the sole non-bridge
  endpoint); with none, it stays idle. Dropdown's first option is
  "Auto (only other output)" = `''`.
- **`--list-render-endpoints` now returns `[{name,isBridge}]`** so both the
  companion's auto-resolve and the dropdown's device filtering can exclude the
  controller's own endpoint (`IsKnownBridgeEndpoint`). `cachedRenderEndpoints`
  in the service; refreshed on settings-open (IPC) and lazily on first
  evaluation after connect when auto-resolve is needed.
- **Dropdown opens wider than the button** (`CustomSelect floatingMenu
  floatingMenuMinWidth={320}`, `white-space: nowrap` on menu items) so long
  Windows endpoint names are fully readable; `.headset-audio-fallback-select`
  CSS + a `:has()` rule widening the controls grid when the select is present.
- Debug log lines re-tagged `[AutoSwitchAudio]`.
- Tests: 6 in `bridge-service.test.ts` (added auto-resolve-single-output and
  stays-idle-with-two-outputs), `ipc-contract.test.ts` covers both setters +
  `listRenderEndpointNames`. `vitest` 332/332, `typecheck`, `build:app`,
  `build:audio-helper` green.

### Redesign #1 after first smoke test (2026-09-06)

First smoke test passed cleanly. Reworked per user feedback into the shape
that ships:

- **No toggle** — a single **`CustomSelect` dropdown** ("Auto Route Audio",
  default companion select style) in Bridge Settings > Power & Controller.
  Options: `Disabled` (default) + live active Windows render endpoint names.
  Picking any device enables the feature with that device as the empty-jack
  target; `Disabled` = off. A previously-saved device that isn't currently
  present stays selectable, labelled `<name> (not connected)`.
- Setting is now **`headsetAudioFallbackDevice: string`** (`''` = off),
  replacing `headsetAudioAutoSwitchEnabled: boolean`. No fork-vs-upstream
  default split needed anymore — off by default for everyone; the TV alias
  list is gone (was fork-specific).
- New AudioHelper verb **`--list-render-endpoints`** → JSON array of active
  render endpoint FriendlyNames on stdout. TS wrapper `listRenderEndpoints()`
  returns `[]` on any failure. Companion refreshes the dropdown options each
  time Bridge Settings opens (`bridge:listRenderEndpointNames` IPC).
- Jack-**in** target unchanged: the bridge/controller endpoint.
- Settings popup is now **full window height**
  (`.bridge-settings-preferences-modal { height: calc(100vh - 48px) }`) with
  the two-column body as an internal `overflow-y: auto` flex child so nothing
  clips.
- Debug log lines re-tagged `[AutoRouteAudio]`.
- Tests updated (4 in `bridge-service.test.ts` now drive
  `headsetAudioFallbackDevice`, 1 in `ipc-contract.test.ts` covers the new
  IPC + `listRenderEndpointNames`). Full `vitest` 330/330, `typecheck`,
  `build:app`, `build:audio-helper` all green.

### Implementation log (2026-09-06) — original toggle-based version, superseded above

Done, `npm run typecheck` + full `vitest` (330/330, +5 new) + `build:app` +
`build:audio-helper` all green:

- **AudioHelper (C#)**: new `--set-default-render --device-name "A;B;C"` verb
  (`EndpointManager.SetDefaultRenderEndpointByName`) — first active render
  endpoint whose FriendlyName matches a candidate wins; non-zero exit if none.
  Manually verified against live hardware (switched default TV<->DualSense,
  no-match/empty cases exit non-zero).
- **`audio-helper.ts`**: `setDefaultRenderEndpointByName(names[])` wrapper.
- **`bridge-service.ts`**: `syncHeadsetAudioAutoSwitch()` called each poll
  after `syncControllerPowerSavingState`; `HEADSET_AUDIO_JACK_DEBOUNCE_POLLS
  = 2`, `HEADSET_AUDIO_UNPLUGGED_TARGET_ALIASES = [Beyond TV, Beyo TV, AMD
  High Definition Audio Device]`; `isRemoteSessionActive()` (SESSIONNAME
  `^rdp-`) and persona-transition/restore guards; `correctDefaultRenderIfBridgeWhileJackEmpty()`
  for the "Windows re-promoted the controller" case; instance-method wrappers
  `this.setDefaultRenderBridgeEndpoint` / `this.setDefaultRenderEndpointByName`
  / `this.getDefaultRenderEndpointStatus` so tests can inject.
  `setHeadsetAudioAutoSwitchEnabled()` service method.
- **Settings**: `headsetAudioAutoSwitchEnabled` on `CompanionSettings`,
  `DEFAULT_SETTINGS` (**true** in the fork; flip to false + update
  `settings-store.test.ts` for the upstream PR), `normalizeSettings`.
- **IPC**: `bridge:setHeadsetAudioAutoSwitchEnabled` in `main.ts` + `preload.ts`.
- **UI**: "Route Audio to Controller with Headset" toggle in Bridge Settings >
  Power & Controller.
- **Tests**: 4 new in `bridge-service.test.ts` (plug/unplug + debounce,
  disabled = no-op, empty-jack correction, RDP dormant); 1 new in
  `ipc-contract.test.ts`. Shared `createService` fixture now baselines the
  feature **off** so it doesn't spawn the real helper in unrelated polling
  tests.

**Not yet done**: hardware smoke test (§7 step 5), the `audio_switch.pyw`
dead-code trim (§7 step 6, only after smoke test), doc updates + CUSTOM.md
Feature #2 section (§7 step 7), push (§7 step 8).

---

## 1. The problem, restated accurately

The user's current setup:

- `C:\auto\boot\audio_switch.pyw` — a Windows script (SoundVolumeView wrapper)
  that sets the default render device by "mode" (`headset` / `tv` / `ds` /
  `auto` / `cycle`).
- `C:\auto\boot\devices_list.pyw` — device-connect watcher. **Only** launches
  `audio_switch.pyw headset` when the **UGREEN BT509 headset** connects. It
  does *not* launch it for the DualSense.
- `C:\auto\boot\Devices_checker.pyw` — the PnP watcher that runs
  `devices_list.pyw` on device arrival; it drives TV wake / HDMI switching,
  not audio.

**Finding, confirmed from `C:\auto\log\audio_switch.log`:** every logged
invocation is `mode=headset`. There is **no** log line switching audio to the
DualSense, and the DualSense never even shows up in `Available targets`. The
`DUALSENSE_ALIASES` / `"ds"` branch in `audio_switch.pyw` is dead code that
nothing calls.

**So the DualSense becoming the default audio device is Windows' own
behavior** — a freshly-arrived audio endpoint (especially one that was
default before) gets auto-promoted to default. Nothing in `C:\auto\boot` is
doing it, and there is nothing DualSense-related to *remove* from
`audio_switch.pyw` because it was never wired.

The real fix is therefore not "make the script smarter". It is:

> Only make the controller the default output **while a headset is physically
> plugged into its 3.5 mm jack**, and force the output to a **fixed fallback
> device (the TV / AMD HDMI endpoint)** when nothing is in the jack — even
> though Windows itself would otherwise leave/make the DualSense default.

### Exact expected behavior (from the user)

Driven purely by the DualSense input report bit `/*53.0*/ uint8_t
PluggedHeadphones : 1` (already surfaced to the companion as
`audioStatus.headsetPlugged`, see §2):

| Jack state | Desired Windows default render device |
|---|---|
| Something plugged into the controller's 3.5 mm jack | **DualSense** (the bridge endpoint) |
| Nothing plugged in | **TV / AMD HDMI** — the first *active* device matching `["Beyond TV", "Beyo TV", "AMD High Definition Audio Device"]` (same alias list `audio_switch.pyw` uses for `tv`) |

Note this is **not** a "restore what was default before" model. The user's
point: Windows may have *already* promoted the DualSense to default (on pair /
connect), so there is often no meaningful prior device to restore to. Instead
the unplugged state has a **fixed target** — the TV endpoint. Simpler and
more predictable than snapshotting.

### Confirmed device state (DualSense connected, no RDP, 2026-09-06)

`SoundVolumeView /sjson`, render endpoints only:

| Endpoint | `Name` | `Device Name` | State | Default |
|---|---|---|---|---|
| DualSense | `Speakers` | `4- DualSense Wireless Controller` | Active | **Render (default)** — nothing in the 3.5 mm jack |
| TV / AMD | `4 - Beyo TV` | `2- AMD High Definition Audio Device` | Active | — |

Observations that settle the design:

- **Windows *did* make the DualSense (`Speakers` / `DualSense Wireless
  Controller`) the system default with an empty jack** — this is the exact
  behavior the feature exists to correct. So §4c step 4 (actively pull the
  default off the DualSense on connect with no headset) **is required**, not
  optional.
- The **TV/AMD endpoint stays `Active` while idle** — so "jack empty → first
  active TV-alias match" will reliably find it. (Still verify it doesn't go
  `Unplugged` when the TV is fully powered off — keep the "none active →
  leave as-is + log" fallback.)
- `EndpointManager`'s existing bridge aliases include `"DualSense Wireless
  Controller"` and `"Wireless Controller"` → `Device Name` `4- DualSense
  Wireless Controller` matches. `--set-default-render-bridge` /
  `IsKnownBridgeEndpoint` should already resolve this endpoint with no new
  alias needed. **Verify during impl** (the `Name` is the generic
  `"Speakers"`, so matching must be on `Device Name`, which
  `EndpointNameMatchesAlias` already does — it checks both).
- New `--set-default-render` verb should match the **TV alias list against
  `Device Name` as well as `Name`** (the match target here is `2- AMD High
  Definition Audio Device` in `Device Name`; `Name` is `4 - Beyo TV`).

---

## 2. Key discovery — the companion app already has the signal

`companion/src/shared/protocol.ts` **already parses headset-jack state** out
of the firmware's audio status report:

```ts
// AudioStatusPayload
headsetPlugged: (routeFlags & 0x01) !== 0,     // report[10] bit 0
headsetAudioRoute: (routeFlags & 0x02) !== 0,  // report[10] bit 1
```

This is the firmware surfacing the DualSense input report's
`PluggedHeadphones` bit (input report byte 53, bit 0 — the same bit issue
#254's answer points at). It is **upstream code, already shipping** — updated
on every audio-status poll, already consumed in the renderer as
`headsetOutputDetected` (`App.tsx:3898`) and in the power-saving gate
(`bridge-service.ts:2205`).

**Consequence: no firmware change is needed for this feature.** The plug/unplug
edge is already available in `bridge-service.ts` via `this.audioStatus.headsetPlugged`.

### Firmware source read — what the two bits mean (§6.6 RESOLVED)

Traced through `src/`:

- **`headset_plugged` (route-flags bit 0)** ← `plug_headset` (`audio.cpp:1707`)
  ← `set_headset(state)` (`audio.cpp:423`) ← **`set_headset((controller_report[53]
  & 1) != 0)` in `main.cpp:519`**, called on every DualSense interrupt-in
  report in `on_bt_data()`. This is a **direct, unfiltered pass-through of
  input-report byte 53 bit 0** — the raw `PluggedHeadphones` bit. `set_headset`
  early-returns when the value is unchanged; there is **no debounce, no
  hysteresis, no timer** anywhere on this path.
- **`headset_audio_route` (route-flags bit 1)** = `speaker_route_active &&
  speaker_route_headset` (`audio.cpp:1708`) — this is "the firmware's speaker
  output path is up **and** currently routed for a headset". It follows
  `plug_headset` but only while an active USB speaker stream exists, and lags
  it (it's set from `plug_headset` inside the route-rearm logic). It's a
  *firmware-internal routing state*, not a cleaner jack signal.

**Decision:** gate on **bit 0 (`headsetPlugged`)** — it's the true jack
state. Do **not** use bit 1 (it's false whenever nothing is playing audio,
which is most of the time). **The companion must add its own debounce**
(§6.7 RESOLVED — there is none upstream, on either side).

## 3. Key discovery — the companion app already has the switching machinery

`companion/native/AudioHelper` (the C# helper) already:

- sets an arbitrary device as the default render endpoint via the undocumented
  `IPolicyConfig::SetDefaultEndpoint` COM call (`EndpointManager.SetDefaultRenderEndpoint`,
  private) — for Console + Multimedia roles;
- has `--set-default-render-bridge [--bridge-persona <mode>]` and
  `--default-render-status` CLI verbs (`Program.cs:2968,2971`);
- can identify "the bridge / DualSense endpoint" by container ID and by name
  alias (`IsKnownBridgeEndpoint`, `FindKnownBridgeEndpoint`);
- can resolve an arbitrary endpoint by friendly-name substring
  (`SelectNamedEndpoint`).

And `bridge-service.ts` already has a **complete pattern** for "switch the
default render endpoint, then restore it later, with retries and deadlines"
— `defaultRenderIsBridgeEndpoint()` / `queueHostPersonaDefaultRenderRestore()`
/ `restoreHostPersonaDefaultRenderIfReady()` (lines ~1748–1865), currently
triggered by host-persona switches.

**Consequence: feature #2 is almost entirely wiring existing pieces together,
triggered by a new signal (headset plug edge) instead of persona changes.**

---

## 4. What actually needs to be built

### 4a. Native helper — one small new verb (C#)

`companion/native/AudioHelper/`:

- **`--set-default-render --device-name "<substr>"`** — resolve an *active*
  render endpoint by name substring (reuse `SelectNamedEndpoint` /
  `EnumerateAudioEndPoints(Render, Active)`) and call the existing private
  `SetDefaultRenderEndpoint(device)`. ~15 lines in `Program.cs` + a thin
  `EndpointManager.SetDefaultRenderEndpointByName(name)` public wrapper.
  Should accept multiple candidate substrings (the TV alias list) and take
  the first match, or the caller loops — decide during impl.
  - Rationale: `--set-default-render-bridge` can only target the bridge. The
    "jack empty" state needs "set default to the TV/AMD endpoint by name".
  - Exit non-zero (so the TS wrapper's retry logic sees it) if no active
    endpoint matches.
- **`--default-render-status`** already returns `{ deviceName,
  isBridgeEndpoint }` — used for the "did Windows promote the DualSense on
  connect?" check in §4c step 4. No change.

Everything else native-side already exists.

### 4b. `audio-helper.ts` — one thin wrapper (TS)

- `setDefaultRenderEndpointByName(name: string): Promise<void>` → spawns
  `--set-default-render --device-name name`. Used only for the "jack empty →
  TV" direction.
- (already have) `getDefaultRenderEndpointStatus()`,
  `setDefaultRenderBridgeEndpoint(mode)` — used for the "jack plugged →
  bridge" direction and the connect-time promotion check.

### 4c. `bridge-service.ts` — the feature logic

New, self-contained block modeled on the host-persona render-restore code.
**Fixed-target model** (not snapshot/restore):

1. **Edge detection + debounce**: `readAudioStatus()` runs every ~500 ms
   (`AUDIO_STATUS_READ_INTERVAL_MS`). Track `lastActedHeadsetPlugged` (the
   value the feature last drove a switch for) and a small
   `pendingHeadsetPlugged` + `pendingSince` / count. Only act when a *new*
   value has held across **≥2 consecutive reads (~1 s)** — firmware does no
   debouncing at all (byte 53 bit 0 is a raw pass-through, see §2), so a
   flaky plug can chatter. Hook this off the existing
   `publishAudioDiagnosticsSnapshot()` / `readAudioStatus()` path, not a new
   timer.
2. **On rising edge (headset plugged in)**, if `settings.headsetAudioAutoSwitchEnabled`:
   - `setDefaultRenderBridgeEndpoint(hostPersonaMode)` → route to the
     controller (with retry + deadline, reuse the `queue…/…IfReady` shape).
   - Log `[HeadsetAudio] jack plugged → default render = bridge`.
3. **On falling edge (headset unplugged)**, if enabled:
   - `setDefaultRenderEndpointByName(<first active TV alias>)` → route to the
     TV/AMD HDMI endpoint (with retry + deadline).
   - Resolve the alias list `["Beyond TV", "Beyo TV", "AMD High Definition
     Audio Device"]` against currently-active render endpoints; use the first
     match. If **none** is active (TV fully off and its endpoint went
     `Unplugged`), fall back to `GetDefaultAudioEndpoint(Multimedia)` /
     leave as-is and log — don't loop.
   - Log `[HeadsetAudio] jack empty → default render = '<name>'`.
4. **On controller connect with an empty jack** (no rising edge, but Windows
   may have just promoted the DualSense): if enabled and the current default
   *is* the bridge endpoint, run the same "route to TV alias" step once.
   (Confirm this is wanted after seeing real connect behavior in the smoke
   test — §5, §6.)
5. **On the feature toggle flipping on**: evaluate current `headsetPlugged`
   and apply the matching target immediately, so enabling it doesn't wait for
   the next plug/unplug.
6. **Guards**:
   - Does nothing unless `settings.headsetAudioAutoSwitchEnabled`.
   - Does nothing while an **RDP session is active** (mirror
     `Devices_checker.pyw`'s `is_rdp_active()` — RDP redirects audio to
     "Remote Audio" and we must not fight it). Re-evaluate on RDP
     disconnect.
   - Does nothing while a host-persona transition / persona render-restore is
     in flight (check the same flags that code checks).
   - Does nothing if the controller is disconnected (no `audioStatus`).
   - Never blocks a command path — all helper calls are background + retry,
     same as the persona restore.
   - The TV-alias list should be a shared constant (not re-typed); consider
     lifting it from a spot both the app and any docs can point at.

### 4d. Settings + protocol plumbing (mirrors the WOL feature's shape)

- `types.ts`: `headsetAudioAutoSwitchEnabled: boolean` on `CompanionSettings`.
- `settings-store.ts`: default (`true`? — decide in §6) + `normalizeSettings`.
- **This is a companion-only preference — no firmware command, no
  `COMMAND_ID`, no protocol-minor bump.** The firmware already sends
  everything needed (`headsetPlugged`). This is purely how the *app* reacts
  to an existing signal, like the power-saving toggle. That keeps the
  upstream PR tiny and side-effect-free on the firmware.
- `main.ts` + `preload.ts`: one `bridge:setHeadsetAudioAutoSwitchEnabled` IPC
  pair.
- `App.tsx`: a toggle in Bridge Settings, near the existing audio / power
  rows. Label e.g. **"Route audio to controller only with headset"**, helper
  text explaining it switches the Windows default output to the controller
  while a headset is in the 3.5 mm jack and restores it after.
- `styles.css`: none expected (reuses `.settings-menu-row`).

### 4e. Tests

- `bridge-service.test.ts`: rising edge switches to bridge and records the
  prior device; falling edge restores it; disabled setting → no calls;
  prior-device-missing → fallback; manual override respected. The existing
  `audioStatusReport({ headsetPlugged })` fixture already exists — reuse it.
- `ipc-contract.test.ts`: the new `setHeadsetAudioAutoSwitchEnabled` IPC is
  wired through preload + main + service (same test shape as the WOL one).
- AudioHelper: a smoke check of the new `--set-default-render` verb if the
  helper has a test harness; otherwise manual.

---

## 5. Removing the responsibility from `C:\auto\boot`

Per the user: **plan the removal now, do it after the smoke test**, and
**only touch `audio_switch.pyw`** — `devices_list.pyw` runs other automations
that stay.

- `devices_list.pyw`: **no change**. It never switched the DualSense; its
  UGREEN-headset branch and all its non-audio work are unaffected.
- `audio_switch.pyw`: after feature #2 is verified on hardware, delete the
  dead DualSense path — `DUALSENSE_ALIASES`, the `"ds"` entry in
  `DEVICE_ORDER`, the `ds`/`dualsense`/`controller` mode branch, and the
  `ds` fallbacks in `auto`/`cycle`. Keep `tv` and `headset` modes exactly as
  they are. Net effect: the script stops being able to target the controller
  at all, which is correct once the companion app owns that.
- No new Task Scheduler entries. The companion app (already autostarted in
  the tray) is the trigger.
- If Windows still auto-promotes the DualSense on connect *before* a headset
  is plugged (the original complaint), the companion's rising/falling-edge
  logic will already pull it back: on connect with no headset, there's no
  rising edge, so nothing routes to it; if Windows promoted it anyway, add a
  small "controller connected, no headset → if default is bridge and we
  didn't put it there, restore previous" correction to §4c step 4. Decide
  after observing real behavior in the smoke test.

---

## 6. Decisions

1. ~~Default state of `headsetAudioAutoSwitchEnabled`~~ **RESOLVED**: **`true`
   in this fork** (`DEFAULT_SETTINGS`), **`false` for the eventual upstream
   PR** (flip that one line + the test expectation when preparing the PR
   branch — note it in the PR description as the intended upstream default).
2. ~~Restore-target semantics~~ **RESOLVED**: fixed target, not a snapshot.
   Jack empty → first *active* endpoint matching (by `Name` **or** `Device
   Name`) the TV alias list `["Beyond TV", "Beyo TV", "AMD High Definition
   Audio Device"]`; if none active, leave default as-is + log. Jack plugged →
   the bridge endpoint.
3. ~~Connect-with-empty-jack correction~~ **RESOLVED — required.** The live
   device dump confirmed Windows makes the DualSense default with an empty
   jack. On controller connect (or feature-enable) with `headsetPlugged ==
   false`, if the current default *is* the bridge endpoint, route to the TV
   alias. This is §4c step 4.
4. **Roles to set** — **RESOLVED (keep default).** Helper sets Console +
   Multimedia. Leave Communications (role 2) alone so chat apps aren't
   yanked onto the controller/TV. Revisit only if the user asks.
5. ~~Interaction with the UGREEN BT509 flow~~ **RESOLVED**: jack unplug
   **always → TV**, never UGREEN. `devices_list.pyw` already fires an audio
   switch when the UGREEN headset *connects*, so that path stays owned by the
   boot script; this feature only ever targets the TV alias list (no UGREEN
   entry) for the empty-jack state.
6. ~~bit 1 vs bit 0~~ **RESOLVED**: gate on **bit 0 `headsetPlugged`** (raw
   jack state). Bit 1 `headsetAudioRoute` is firmware-internal routing state,
   false whenever nothing is playing — unusable. See §2 source read.
7. ~~Debounce~~ **RESOLVED**: none exists upstream (firmware byte-53 bit 0 is
   a raw pass-through; companion power-saving gate reads the raw bit). Add
   one: act only after a changed value holds **≥2 consecutive ~500 ms audio
   polls**. See §4c step 1.
8. ~~Exact endpoint strings~~ **RESOLVED** — captured 2026-09-06, see the
   boxed table in §1.

---

## 7. Implementation order

1. Resolve §6 decisions **1** (default on/off) and **5** (unplug → TV only,
   or UGREEN-then-TV) with the user. Everything else is resolved.
2. AudioHelper: add `--set-default-render --device-name` (accepts repeated
   values, first active `Name`/`Device Name` match wins, non-zero exit if
   none). Verify `--set-default-render-bridge` already resolves the
   `4- DualSense Wireless Controller` endpoint (it should via existing
   aliases). Build (`npm run build:audio-helper`), manual-test both verbs.
3. `audio-helper.ts` wrapper + `bridge-service.ts` feature block (edge +
   debounce + fixed-target switch + connect/enable correction + RDP/persona
   guards) + settings plumbing (`types.ts`, `settings-store.ts`) + IPC
   (`main.ts`, `preload.ts`) + `App.tsx` toggle.
4. `npm run typecheck` + `npx vitest run src` (new + existing tests green).
5. `.\tools\rebuild-companion.ps1`, smoke test:
   - No headset, connect controller → default forced to **TV** (`4 - Beyo
     TV`), not left on the DualSense.
   - Plug headset into controller jack → default switches to **DualSense**
     (`Speakers`) within ~1–1.5 s (debounce).
   - Unplug → default switches back to **TV**.
   - Toggle the feature off → neither switch happens; toggle back on →
     immediately applies the current jack state.
   - Flaky/repeated half-insert → no rapid flip-flopping (debounce holds).
   - Start an RDP session → feature goes dormant, no fighting Remote Audio.
6. Only then: trim the dead DualSense path from `audio_switch.pyw` (§5) —
   `DUALSENSE_ALIASES`, the `"ds"` `DEVICE_ORDER` entry, the
   `ds`/`dualsense`/`controller` mode branch, `ds` fallbacks in
   `auto`/`cycle`. Leave `devices_list.pyw` and everything else untouched.
7. Update CHANGELOG.md / DECISIONS.md (the "no firmware change — bit 0 is a
   raw pass-through" rationale, the fixed-target-not-snapshot decision, the
   RDP guard, the debounce). Fold a port section into CUSTOM.md as
   "Feature #2" — it's a candidate upstream PR too, and the fork needs the
   same re-apply map.
8. Push `feature/wol-wifi` → `origin/wol-wifi-full-history` (the fork's
   working-history branch; `origin/feature/wol-wifi` stays the clean WOL PR
   branch).

---

## 8. Why this is a good upstream PR

- **Zero firmware change** — consumes a bit the firmware already sends.
- Reuses the existing `IPolicyConfig` switching + the existing
  render-restore state-machine shape.
- One companion setting, one small C# verb, no protocol surface, no
  `COMMAND_ID`, no version bump.
- Directly answers a real user request (#254) that the maintainer already
  said needs "a small Windows application" — the companion app *is* that
  application, and already has 90% of the plumbing.
