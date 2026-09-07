Title: feat(companion): auto-switch Windows audio output on controller headset jack

---

## Problem

The DualSense's audio endpoint stays present in Windows whether or not
anything is plugged into the controller's 3.5 mm jack. Windows frequently
promotes it to the default output device when the controller connects, so
system audio goes to the controller's speaker even with no headset attached.

Windows cannot detect the jack state itself — the USB audio device does not
disconnect when the headset is unplugged — so it cannot switch back on its
own. Users have to change the default output manually every time.

## Change

Companion-app only. The firmware already reports the jack state: the
DualSense input report's `PluggedHeadphones` bit (byte 53, bit 0) reaches the
companion as `audioStatus.headsetPlugged`. No firmware change is required.

When enabled, the companion drives the Windows default render endpoint:

- Headset in the controller's jack → the controller/bridge endpoint.
- Jack empty → a fallback output device.

The fallback is chosen in Bridge Settings. When exactly one non-controller
output exists it is used automatically and no dropdown is shown; with two or
more, a dropdown lists them. This also covers the case where Windows promotes
the controller to default while the jack is empty — the companion routes back
to the fallback.

### Details

- **Debounce.** The firmware bit is an unfiltered pass-through of the input
  report with no debouncing, so a marginal plug can chatter. A changed jack
  state must hold across two consecutive audio-status polls (~1 s) before the
  companion acts.
- **Bit 0, not bit 1.** `headsetAudioRoute` (bit 1) reflects firmware-internal
  speaker routing and is false whenever nothing is playing. `headsetPlugged`
  (bit 0) is the actual jack state.
- **Dormant during RDP.** Windows redirects audio to a Remote Audio endpoint
  during a remote session; the feature stays inactive rather than fighting it.
- **Dormant during host-persona transitions** and while the existing
  persona default-render restore is in flight.
- Off by default. Enabling requires the toggle, and takes effect only when a
  fallback device can be resolved.

### Implementation

- `companion/native/AudioHelper` — two new verbs:
  `--set-default-render --device-name "A;B;C"` (set the default render
  endpoint to the first active match; reuses the existing
  `IPolicyConfig::SetDefaultEndpoint` path) and `--list-render-endpoints`
  (active render endpoints as JSON, flagging the bridge's own endpoint).
- `bridge-service.ts` — `syncHeadsetAudioAutoSwitch()`, called once per poll
  after `syncControllerPowerSavingState()`.
- Two settings: `headsetAudioAutoSwitchEnabled` (bool) and
  `headsetAudioFallbackDevice` (string; empty means auto-resolve).
- Three IPC channels, one settings row in Bridge Settings.

No `COMMAND_ID`, no `PROTOCOL_MINOR` bump, no firmware change.

## Testing

`npm run typecheck` and `npx vitest run src` pass. Six new tests cover the
plug/unplug transitions with debounce, the disabled case, auto-resolution of a
single output, the ambiguous multi-output case, the empty-jack correction, and
RDP dormancy. One test covers the IPC contract.

Hardware (Waveshare RP2350B-Plus-W, Windows 11):

1. Controller connects with an empty jack, Windows promotes it to default —
   output is routed back to the fallback device.
2. Headset plugged in — output switches to the controller within ~1 s.
3. Unplugged — output returns to the fallback.
4. Feature disabled — neither switch occurs.
5. Repeated partial insertions — no flip-flopping.
