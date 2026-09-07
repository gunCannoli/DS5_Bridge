Title: fix(bt): don't idle-disconnect while audio is playing to the controller

---

## Problem

Idle Disconnect drops the controller after its timeout based only on the
absence of meaningful HID input. Audio playback is not considered.

Using the controller as a headset — a headset plugged into its 3.5 mm jack,
watching a movie or listening to music — means no button input for long
stretches. The controller idle-disconnects mid-playback and the audio cuts
out. The only workaround is disabling Idle Disconnect entirely or setting an
impractically long timeout.

## Change

One condition in `src/bt.cpp`, in the inactivity check in
`l2cap_packet_handler_cold()`:

```cpp
    // Inactivity detection.
    if (mute[1]) { // Microphone mute is enabled.
        return;
    }
    if (audio_output_route_protected()) {
        inactive_time = now_us;
        return;
    }
    if (!meaningful_input_activity && now_us - inactive_time > idle_disconnect_timeout_us()) {
        ...
    }
```

`audio_output_route_protected()` (`audio_recent() || usb_speaker_streaming_active()`)
already exists in the same file and is already used for the RSSI idle gate and
output-route protection. This reuses it rather than adding a new signal.

The idle clock is reset (rather than simply returning) so the full timeout
restarts when playback stops — matching how a button press resets it.
Returning without the reset would leave a long playback session about to trip
the timeout the moment audio ends.

Placement is deliberate: after the existing `mute[1]` mic-mute carve-out,
before the timeout comparison. It mirrors that carve-out in both location and
shape.

## Scope

- No new state, no timer, no allocation.
- No companion command, no protocol change, no `COMMAND_ID`, no
  `PROTOCOL_MINOR` bump.
- No UI change. The existing Idle Disconnect enable toggle and timeout
  selector behave exactly as before; they simply stop firing during playback.
- Runs only on HID interrupt packets, the same cadence as the existing check.

## Testing

Host-side (`./boards/run_firmware_tests.sh`): all suites pass. The RSSI-idle
source assertion in `tests/firmware/usb_descriptor_migration_test.cpp` was
extended to require the new guard, its exact body, and its ordering between
the mic-mute carve-out and the timeout comparison.

Hardware (Waveshare RP2350B-Plus-W, Idle Disconnect set to 1 minute):

1. Headset in the jack, continuous playback, no controller input — stays
   connected well past the timeout, audio uninterrupted.
2. Playback stopped, still no input — disconnects roughly one timeout period
   after audio stopped, not immediately.
3. No audio and no input — disconnects at the timeout, unchanged from before.
4. Mic mute still independently suppresses idle disconnect.
