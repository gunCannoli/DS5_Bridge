// LOCAL-ONLY: fork patch, not part of upstream's Wake-on-LAN. See Features-Port.md 7a.
#pragma once

#include <cstdint>

// Lightbar feedback for Wake-on-LAN, shown only while the controller is
// connected and Wi-Fi is not using the radio (the quiet gate drops controller
// output during the join/send/teardown window, so nothing can show then).
//   Waiting: a wake packet was submitted and the PC has not enumerated the
//            bridge: flashing green.
//   Alive:   the PC enumerated the bridge outside its firmware: solid green.
//   Off:     restore the lightbar color that was showing before.
// The sequence ends 45 s after the PC returns (the engine's host-returned rule)
// or 5 min after the packet if the PC never comes up.
enum class WolIndicatorMode : uint8_t { Off, Waiting, Alive };

// Pure decision, kept separate so the host tests can cover it.
constexpr WolIndicatorMode wol_indicator_mode_for(bool host_starting, bool host_counted) {
    return !host_starting ? WolIndicatorMode::Off
        : host_counted ? WolIndicatorMode::Alive : WolIndicatorMode::Waiting;
}

// Reads the engine and USB state, then drives the lightbar. Call from the main
// loop's Lightbar phase, only while the quiet gate is open.
void wol_indicator_service();
