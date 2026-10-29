# Plan — Feature #3: make Idle Disconnect audio-aware

Status: **implemented, pending hardware smoke test** (2026-09-06). Third
custom feature for this fork. Separate upstream PR (this one is
**firmware-side**, like WOL — not companion-only like the audio-switch
feature).

### Implementation log (2026-09-06)

Option A (silent, no UI) — confirmed by the user. Done:

- **`src/bt.cpp`**: added, in `l2cap_packet_handler_cold()`'s idle-disconnect
  block, after `if (mute[1]) return;` and before the timeout comparison:
  ```cpp
  if (audio_output_route_protected()) {
      inactive_time = now_us;
      return;
  }
  ```
- **`tests/firmware/usb_descriptor_migration_test.cpp`**: extended the
  existing RSSI-idle source-text assertion to also require the idle block to
  contain `if (audio_output_route_protected())` with the exact
  `inactive_time = now_us; return;` body, ordered after `if (mute[1])` and
  before the `idle_disconnect_timeout_us()` comparison.
- `./boards/run_firmware_tests.sh` → all suites pass (49 logic + descriptor
  guard + diagnostics). `.\tools\build-firmware.ps1` → clean, staged
  `firmware/ds5-bridge-1.71-wol-final.uf2`.

**Remaining**: hardware smoke test (§6), then the standalone firmware PR.

Tracks the user scenario: *watching a movie with a headset plugged into the
DualSense, no button input for 15 min → the controller idle-disconnects and
kills the audio.* Idle Disconnect today only knows "no meaningful input
report activity"; it should also treat **audio actively playing to the
controller** as the device being in use.

---

## 1. How Idle Disconnect works today (firmware)

`src/bt.cpp`, in `l2cap_packet_handler_cold()` on every HID interrupt-in
packet (`channel == hid_interrupt_cid`), ~line 4162:

```cpp
const uint64_t now_us = time_us_64();
const bool meaningful_input_activity = controller_input_report_is_active(packet, size);
if (meaningful_input_activity) {
    inactive_time = now_us;
    arm_signal_strength_idle_epoch(now_us);
}

// Inactivity detection.
if (mute[1]) { // Microphone mute is enabled.
    return;
}
if (!meaningful_input_activity && now_us - inactive_time > idle_disconnect_timeout_us()) {
    DS5_LOG("disconnect when inactive\n");
    inactive_time = now_us;
    bt_disconnect_with_intent(BtControllerDisconnectIntentIdleTimeout);
}
```

- `inactive_time` is reset to "now" only on meaningful *input*. Audio
  playback, headphone use, etc. don't touch it.
- There is **already one non-input carve-out**: `if (mute[1]) return;` — when
  the controller's mic is muted, idle disconnect is suppressed entirely.
  Precedent for "a non-input condition suppresses idle disconnect".
- Timeout is `bt_idle_disconnect_timeout_minutes()` (1–120, default 15),
  set from the companion (`SET_IDLE_DISCONNECT_TIMEOUT` `0x1C`), enable via
  `SET_IDLE_DISCONNECT_ENABLED` `0x03`.

## 2. The signal already exists in firmware

`src/bt.cpp` line ~4959:

```cpp
static bool audio_output_route_protected() {
    return audio_recent()                    // audio.cpp: audio sent to the controller within SPEAKER_USB_SILENCE_TAIL_US (500 ms)
        || usb_speaker_streaming_active();    // usb.cpp: the Windows USB audio OUT endpoint is actively streaming
}
```

Forward-declared at bt.cpp:376, already used in three places (RSSI idle
gating ~line 2409, output-route protection ~line 5372, ~line 1580). It is
**exactly** the "is audio playing to the controller right now" check this
feature needs, and it's already in the same translation unit as the idle
check.

- `audio_recent()` (`audio.h:73`) — `last_audio_us != 0 && now - last_audio_us
  < 500 ms`. Bridges brief silences between audio packets (movie dialogue
  gaps, etc.).
- `usb_speaker_streaming_active()` (`usb.h:29`) — true for the whole duration
  the OS holds the audio endpoint open for playback, even across longer
  silences (paused video with the endpoint still open, quiet scenes). This is
  the one that really covers "watching a movie".

## 3. The change — one condition in one place (firmware)

`src/bt.cpp`, the idle-disconnect block above:

```cpp
    // Inactivity detection.
    if (mute[1]) { // Microphone mute is enabled.
        return;
    }
    // Audio actively routed to the controller (movie/music through the
    // headset jack) means the device is in use even with no button input --
    // don't idle-disconnect and cut the audio. Same signal the RSSI idle
    // gate and output-route protection already use.
    if (audio_output_route_protected()) {
        inactive_time = now_us;   // keep the idle clock from expiring while audio plays
        return;
    }
    if (!meaningful_input_activity && now_us - inactive_time > idle_disconnect_timeout_us()) {
        ...
    }
```

Design notes:

- **Reset `inactive_time = now_us`** (not just `return`) so that when audio
  finally stops, the full idle timeout starts fresh from that moment —
  matching how a button press resets it. Without the reset, audio stopping
  after a 2-hour movie would immediately trip the timeout on the next
  input-less packet.
- Placed **after** the `mute[1]` early-return (keep that behavior exactly)
  and **before** the timeout comparison — minimal, localized, one new `if`.
- No new state, no timer, no companion command, no protocol change. The
  check runs only on HID interrupt packets, same cadence as today.
- `meaningful_input_activity` still resets `inactive_time` above this block
  as before — audio-aware is purely *additional* protection.

## 4. Companion app — surface it, or silent?

**Option A (recommended): silent, no UI.** The behavior is strictly "don't
disconnect the device while it's actively playing audio" — there's no
downside a user would want to opt out of, and it matches the existing
`mute[1]` carve-out which also has no UI. Idle Disconnect's existing toggle +
timeout stay exactly as they are; they just become correct.

**Option B: a sub-line under Idle Disconnect** ("Stay connected while audio
is playing", default on). More discoverable but adds a `COMMAND_ID` + setting
+ protocol bump for something that should always be on. Only worth it if a
reviewer asks for the escape hatch.

Plan assumes **A**. If the upstream reviewer wants it configurable, it
becomes a `bt_set_idle_disconnect_audio_aware(bool)` + one command, same
shape as `SET_IDLE_DISCONNECT_ENABLED`.

## 5. Files touched

Firmware only (Option A):

| File | Change |
|---|---|
| `src/bt.cpp` | The one `if (audio_output_route_protected()) { inactive_time = now_us; return; }` block in the idle-disconnect path. Nothing else. |

That's the whole feature. `audio_output_route_protected()`, `audio_recent()`,
`usb_speaker_streaming_active()` all already exist and are already linked.

If Option B is required later: `src/bt.cpp` (a `bool
idle_disconnect_audio_aware` + setter/getter), `src/companion.cpp` (new
`CommandId` + handler + reapply), `companion/src/shared/protocol.ts`
(`COMMAND_ID` + `PROTOCOL_MINOR` bump), `companion/src/shared/types.ts`,
`settings-store.ts`, `bridge-service.ts`, `main.ts`, `preload.ts`, `App.tsx`,
tests — i.e. the full WOL-style protocol-plumbing set.

## 6. Testing

- **Host-side test** (`tests/firmware/usb_descriptor_migration_test.cpp`):
  this suite does **source-text assertions**, not runtime — it greps the
  actual `src/*.cpp` for expected patterns (it already asserts
  `bt_signal_strength_loop` contains `audio_recent()` /
  `usb_speaker_streaming_active()` at lines ~1244, and that a block near
  ~2102 references `audio_output_route_protected`). Add an assertion that the
  idle-disconnect block in `bt.cpp` (locate by `idle_disconnect_timeout_us()`
  or `BtControllerDisconnectIntentIdleTimeout`) contains
  `audio_output_route_protected()` guarding the disconnect. Same style as the
  existing checks — no runtime stubbing needed (`bt.cpp` isn't compiled by
  the test binaries).
- **Hardware smoke test**:
  1. Headset in the DualSense jack, play a long video (YouTube/movie), no
     controller input. Set Idle Disconnect to 1 min to speed the test.
     Expect: controller stays connected well past 1 min, audio uninterrupted.
  2. Stop playback, no input. Expect: controller idle-disconnects ~1 min
     after audio stopped (timeout restarts from audio-stop, not from the
     last button press).
  3. Idle Disconnect still works normally with no audio: no input, no audio,
     1 min → disconnects as today.
  4. Regression: `mute[1]` (mic mute) still independently suppresses idle
     disconnect.
- Build via `.\tools\build-firmware.ps1` (PowerShell), host tests via
  `./boards/run_firmware_tests.sh`. No companion rebuild needed (Option A).

## 7. Upstream PR framing

- Small, self-contained firmware fix: "Idle Disconnect ignores audio
  playback, disconnecting the controller mid-movie when used as a headset."
- One `if`, reusing an existing helper the codebase already trusts for the
  same "audio is live" question in RSSI gating and output-route protection.
- Mirrors the existing `mute[1]` carve-out — same location, same shape.
- No protocol/UI change; existing Idle Disconnect controls unchanged.

## 8. Open questions

1. **Option A vs B** — silent (recommended) vs a companion toggle. Decide
   with the user; default to A.
2. **`inactive_time = now_us` vs bare `return`** — plan uses the reset so the
   timeout restarts when audio stops. Confirm that's the wanted behavior
   (alternative: bare `return`, so a movie that ends leaves the controller
   ~0 s from disconnecting — probably not what anyone wants).
3. **`audio_recent()` 500 ms tail** — is `usb_speaker_streaming_active()`
   alone enough (endpoint-open = protected), making `audio_recent()`
   redundant here? Using the existing combined helper is simpler and
   harmless; keep it unless there's a reason to narrow it.
4. ~~Interaction with other `inactive_time` readers~~ **checked**: the only
   writers are the timeout setter (`bt_set_idle_disconnect_timeout_minutes`),
   the connect path (~4040), meaningful input (~4166), and the disconnect
   itself (~4176) — all "restart the idle clock". Adding an audio-playing
   writer is the same pattern, no conflict. WOL's `ObserveHost` is a
   different code path and doesn't touch `inactive_time`.
