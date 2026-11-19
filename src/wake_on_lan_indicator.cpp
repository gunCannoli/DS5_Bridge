// LOCAL-ONLY: fork patch, not part of upstream's Wake-on-LAN. See Features-Port.md 7a.
#include "wake_on_lan_indicator.h"

#include "bt.h"
#include "pico/time.h"
#include "usb.h"
#include "wake_on_lan.h"

namespace {
constexpr uint32_t kFlashHalfPeriodUs = 500000;

bool active = false;
// The color showing before the indicator started. bt_set_lightbar_color()
// overwrites the normal restore target on every call, so keep our own copy.
uint8_t snap_r, snap_g, snap_b, snap_brightness;
enum class Shown : uint8_t { None, On, Dark, Solid } shown = Shown::None;
uint32_t last_toggle_us = 0;

void end_indicator(bool show_restore) {
    active = false;
    shown = Shown::None;
    if (show_restore) {
        bt_set_lightbar_color(snap_r, snap_g, snap_b, snap_brightness);
    } else {
        // No link (or the host now owns the lightbar): only repair the stored
        // restore color so the next connect does not restore green.
        bt_save_lightbar_color(snap_r, snap_g, snap_b, snap_brightness);
    }
}
}

void wol_indicator_service() {
    const WolIndicatorMode mode = wol_indicator_mode_for(
        wake_on_lan_host_starting(), usb_wake_host_active() && !usb_wake_host_firmware());
    const bool connected = bt_is_controller_connected();
    const bool host_owned = connected && bt_host_lightbar_owned();

    if (mode == WolIndicatorMode::Off || !connected || host_owned) {
        if (active) {
            end_indicator(connected && !host_owned);
        }
        return;
    }
    if (!active) {
        bt_get_lightbar_color(&snap_r, &snap_g, &snap_b, &snap_brightness);
        active = true;
        shown = Shown::None;
    }
    if (mode == WolIndicatorMode::Alive) {
        if (shown != Shown::Solid) {
            bt_set_lightbar_color(0x00, 0xff, 0x00, 100);
            shown = Shown::Solid;
        }
        return;
    }
    const uint32_t now_us = time_us_32();
    if (shown == Shown::None || shown == Shown::Solid
        || static_cast<int32_t>(now_us - last_toggle_us) >= static_cast<int32_t>(kFlashHalfPeriodUs)) {
        const bool turn_on = shown != Shown::On;
        bt_set_lightbar_color(0x00, 0xff, 0x00, turn_on ? 100 : 0);
        shown = turn_on ? Shown::On : Shown::Dark;
        last_toggle_us = now_us;
    }
}
