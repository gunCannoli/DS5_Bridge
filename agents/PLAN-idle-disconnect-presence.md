# Plan — display-aware Idle Disconnect (companion enhancement to PR 146)

Status: **implemented, pending hardware smoke test** (2026-09-07).
Companion-only. Complements PR 146 (firmware audio-aware Idle Disconnect).

Branch `pr/idle-disconnect-display-aware` (one commit off `upstream/main`),
cherry-picked onto `feature/wol-wifi` as fork-local. Staged for a future PR
to DS5_Bridge once PR 146 lands.

**Structure:** a separate companion-side branch off `upstream/main`
(`pr/idle-disconnect-display-aware`), staged for a future PR. Committed to
`feature/wol-wifi` now as fork-local. PR 146's firmware branch is not
touched.

**No new setting, no new toggle.** The existing "Idle Disconnect" toggle
gets smarter. When it is on:

- Screen ON  -> keep the controller connected while audio plays (PR 146).
- Screen OFF (or dimmed) **and** no HID input past the idle timeout ->
  disconnect, even while audio plays.
- Display state unknown (monitor not up, spawn failed) -> behave exactly
  like PR 146 alone: audio keeps the connection. No regression.

## Why display state is the only usable signal

The reporting user's setup rules out the alternatives:

- No session lock on screen-off -> `powerMonitor.on('lock-screen')` never
  fires.
- Watching a movie = no keyboard/mouse for a long time ->
  `getSystemIdleTime()` cannot tell "watching" from "walked away".

Display power state is what separates them. Deliberately turning the screen
off to listen to music also disconnects the controller, but that costs one
button press to reconnect -- acceptable.

## Getting display state

Electron's `powerMonitor` has no display-power event, and
`GUID_CONSOLE_DISPLAY_STATE` is notification-only (no "get current state"
call). So: subscribe via a small native helper.

The existing `--monitor-audio-sessions` helper only runs when audio-reactive
haptics is on, so it is not reusable. New verb:

**`--monitor-display-state`** (`companion/native/AudioHelper`):
- Creates a message-only window, calls
  `RegisterPowerSettingNotification(GUID_CONSOLE_DISPLAY_STATE)`.
- On `WM_POWERBROADCAST` / `PBT_POWERSETTINGCHANGE`, prints one line:
  `display: on` / `display: off` / `display: dimmed`.
- Prints an initial `display: unknown` immediately, then the real state on
  the first notification (Windows sends one on subscribe).
- Exits on stdin `stop` or Ctrl-C. ~90 lines of C#.

## Companion (bridge-service.ts)

- On controller connect, if Idle Disconnect is enabled, spawn
  `--monitor-display-state`; keep it for the session. Stop it on disconnect
  / when Idle Disconnect is turned off. Same lifecycle shape as the other
  helper monitors.
- Track `displayState: 'on' | 'off' | 'dimmed' | 'unknown'` and
  `displayOffSince: number | null` (ms; set when state becomes off/dimmed,
  cleared when on).
- New per-poll method `syncIdleDisconnectDisplayOverride()`, called after the
  existing audio-switch sync:

```
if (!idleDisconnectEnabled || state !== 'connected' || !controllerConnected) return;
if (pcAsleep) return;                          // USB-suspend path already owns this
if (displayState !== 'off' && displayState !== 'dimmed') return;  // screen on / unknown -> PR 146
if (displayOffSince === null) return;
const thresholdMs = idleDisconnectTimeoutMinutes * 60_000;
if (Date.now() - displayOffSince <= thresholdMs) return;
if (powerMonitor.getSystemIdleTime() * 1000 <= thresholdMs) return;
if (this.idleDisconnectDisplayOverrideLatched) return;           // one-shot per screen-off episode
if (<any persona transition / WoL wake / command in flight>) return;

this.idleDisconnectDisplayOverrideLatched = true;
await this.sleepController();     // COMMAND_ID.SLEEP_CONTROLLER 0x11, already wired
```

- Latch cleared when `displayState` returns to `on`.
- `sleepController()` disconnects with `BtControllerDisconnectIntentSleep`.
  No new `COMMAND_ID`, no protocol change, **no firmware change**.
- After the disconnect the controller reconnects on the next button press
  (normal BT reconnect). WoL fires on connect only if the PC is off, which
  it is not, so no interaction.

## Files

| File | Change |
|---|---|
| `companion/native/AudioHelper/DisplayStateMonitor.cs` (new) | the monitor |
| `companion/native/AudioHelper/Program.cs` | `--monitor-display-state` verb + dispatch |
| `companion/src/main/audio-helper.ts` | `DisplayStateMonitorEngine` spawn/lifecycle, emits `display-state` |
| `companion/src/main/bridge-service.ts` | monitor lifecycle tied to Idle Disconnect enabled + connect; `syncIdleDisconnectDisplayOverride()` per poll; `displayState` / `displayOffSince` / latch state |
| tests | bridge-service: screen off + idle past timeout -> `sleepController()`; screen on -> no-op; display unknown -> no-op (PR 146 path); PC asleep -> no-op; latch prevents repeat; toggling Idle Disconnect off stops the monitor |

No `types.ts` / `settings-store.ts` / `main.ts` / `preload.ts` / `App.tsx`
change -- there is no new setting.

## Open questions (resolved)

1. ~~Separate PR vs fold into 146~~ -> **separate companion branch**, local now.
2. ~~Fallback when display state unknown~~ -> **PR 146 behavior** (audio holds).
3. ~~New toggle~~ -> **no**, reuse the existing Idle Disconnect toggle.
4. **Threshold** -> reuse `idleDisconnectTimeoutMinutes`. Require BOTH
   "screen off for > threshold" AND "input idle for > threshold" -- the
   double condition avoids disconnecting someone who just glanced away with
   an aggressive DPMS setting.
5. **`dimmed`** -> treated as off.
