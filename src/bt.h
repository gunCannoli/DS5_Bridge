//
// Created by awalol on 2026/3/4.
// Modified for DS5 Bridge companion firmware and app integration.
//

#ifndef DS5_BRIDGE_BT_H
#define DS5_BRIDGE_BT_H

#include <cstdint>
#include <vector>

enum CHANNEL_TYPE {
    INTERRUPT,
    CONTROL
};

enum ControllerType : uint8_t {
    ControllerTypeUnknown = 0,
    ControllerTypeDualSense = 1,
    ControllerTypeDualSenseEdge = 2,
};

enum BtControllerDisconnectIntent : uint8_t {
    BtControllerDisconnectIntentNone = 0,
    BtControllerDisconnectIntentSleep = 1,
    BtControllerDisconnectIntentIdleTimeout = 2,
};

struct BtDeviceIdentitySnapshot {
    bool address_known;
    bool controller_connected;
    bool link_key_known;
    bool pairing_active;
    uint8_t link_key_type;
    char address[18];
    char name[32];
    uint16_t vendor_id;
    uint16_t product_id;
};

typedef void (*bt_data_callback_t)(CHANNEL_TYPE channel, uint8_t *data, uint16_t len);

int bt_init();
void bt_register_data_callback(bt_data_callback_t callback);
bool bt_is_controller_connected();
uint8_t bt_controller_type();
int8_t bt_get_signal_strength();
bool bt_has_signal_strength();
bool bt_disconnect();
bool bt_disconnect_with_intent(BtControllerDisconnectIntent intent);
bool bt_expected_disconnect_pending();
bool bt_power_off_controller();
bool bt_request_scan();
bool bt_forget_pairings();
bool bt_forget_pairing(uint8_t address[6]);
bool bt_get_device_identity(BtDeviceIdentitySnapshot *snapshot);
bool bt_pairing_active();
bool bt_set_idle_disconnect_timeout_minutes(uint16_t minutes);
uint16_t bt_idle_disconnect_timeout_minutes();
void bt_write(uint8_t* data,uint16_t len);
bool bt_write_classified_output(uint8_t* data,uint16_t len);
bool bt_sanitize_host_speaker_amp_ownership(uint8_t* data,uint16_t len);
bool bt_sanitize_host_speaker_amp_ownership_payload(uint8_t* payload,uint16_t len);
bool bt_sanitize_host_mic_ownership(uint8_t* data,uint16_t len);
bool bt_sanitize_host_mic_ownership_payload(uint8_t* payload,uint16_t len);
bool bt_apply_classic_rumble_gain_payload(uint8_t* payload,uint16_t len);
bool bt_write_audio_stream(uint8_t* data,uint16_t len);
void bt_drain_audio_stream();
void bt_reset_output_debug_stats();
struct bt_output_debug_stats {
    uint32_t audio_0x36_enqueue_to_send_max_us;
    uint32_t audio_0x36_send_gap_max_us;
    uint32_t audio_0x36_late_count_over_12000_us;
    uint32_t audio_0x36_drop_oldest_count;
    uint32_t non_audio_reports_between_audio_max;
    uint32_t bt_audio_queue_depth_max;
    uint32_t audio_0x36_enqueued_count;
    uint32_t audio_0x36_sent_count;
    uint32_t audio_l2cap_send_fail_count;
    uint32_t normal_0x31_rx_count;
    uint32_t normal_0x31_sent_count;
};
void bt_get_output_debug_stats(bt_output_debug_stats *stats);
void bt_set_lightbar_color(uint8_t red, uint8_t green, uint8_t blue, uint8_t brightness_percent);
void bt_set_player_led_enabled(bool enabled);
void bt_set_mute_led(bool enabled);
void bt_set_microphone_state(uint8_t volume_percent, bool muted, bool control_mute_led, bool mute_led);
void bt_set_speaker_output_gain(uint8_t gain);
uint8_t bt_speaker_output_gain();
void bt_set_speaker_output_enabled(bool enabled, bool headset_plugged = false, bool force = false);
void bt_rearm_speaker_output_route(bool headset_plugged);
void bt_refresh_speaker_output();
void bt_set_classic_rumble_gain(uint16_t gain_percent);
uint16_t bt_classic_rumble_gain();
void bt_set_classic_rumble_v1_enabled(bool enabled);
bool bt_classic_rumble_v1_enabled();
void bt_set_classic_rumble_output(uint8_t right, uint8_t left);
void bt_set_adaptive_trigger_effect(uint8_t mode, uint8_t intensity_percent, uint8_t target = 0);
void bt_set_custom_adaptive_trigger_effect(
    uint8_t mode,
    uint8_t start_percent,
    uint8_t wall_percent,
    uint8_t force_percent,
    uint8_t target = 0
);
void bt_set_custom_adaptive_trigger_effects(
    uint8_t right_mode,
    uint8_t right_start_percent,
    uint8_t right_wall_percent,
    uint8_t right_force_percent,
    bool right_active,
    uint8_t left_mode,
    uint8_t left_start_percent,
    uint8_t left_wall_percent,
    uint8_t left_force_percent,
    bool left_active
);
void bt_replay_adaptive_trigger_effect(
    uint8_t const *right_trigger,
    bool right_valid,
    uint8_t const *left_trigger,
    bool left_valid,
    uint8_t motor_power,
    bool motor_power_valid
);
void bt_reset_adaptive_triggers();

// Board-level WOL/connection trace: a small ring buffer recording BT
// connection-phase transitions, disconnect reasons/timeouts, and wolwifi.cpp
// events, so a WOL attempt that happens while the target PC (and therefore
// the companion app) is off can still be diagnosed once the app reconnects
// -- unlike the live-only WOL debug log and firmware UART log, which both
// require a companion connection at the moment the event happens. See
// decisions.md: added after "controller connects then drops ~15s later,
// WOL never starts" couldn't be diagnosed from either of those because the
// target PC was off the whole time.
enum class WolTraceStage : uint8_t {
    ConnPhaseConnecting = 0,
    ConnPhaseSecuring = 1,
    ConnPhaseHidOpening = 2,
    ConnPhaseReady = 3,
    ConnPhaseDisconnecting = 4,
    ConnSecurityTimeout = 5,
    ConnHidOpeningTimeout = 6,
    ConnHidInterruptFollowupTimeout = 7,
    ConnDisconnected = 8,
    WolTriggerFired = 9,
    WolTriggerSkipped = 10,
    // 11 was WolConnectDelayStart (removed -- WOL now starts the Wi-Fi
    // connect immediately on a controller-connect edge, no pre-delay; see
    // decisions.md). Left unassigned rather than renumbering the stages
    // after it, so old trace records/log lines from prior firmware still
    // decode to the same stage names.
    WolResendBegin = 12,
    WolResendConfirmed = 13,
    WolResendGaveUp = 14,
    // Added to correlate BT disconnects (reason 0x22 in particular) against
    // what wolwifi.cpp's Wi-Fi connect sequence was doing at the same time --
    // previously only visible in the live-only WOL debug log via
    // raw_link_status/dhcp_state, unavailable for a host-off attempt. detail
    // is the elapsed ms in that phase (capped to 255) for both.
    WolWifiAssocTimeout = 15,
    WolDhcpWaitTimeout = 16,
    // Appended once at boot if the prior boot ended in a watchdog reboot
    // (see watchdog_telemetry.h) -- a stall anywhere in the main loop for
    // over the 1000ms watchdog timeout silently reboots the board with no
    // warning otherwise, which would look identical to "nothing happened"
    // in every other log (all live-only, and the reboot itself wipes the
    // WOL trace ring along with all other RAM state). detail is the
    // WatchdogMainLoopPhase the board was stuck in when it rebooted (see
    // watchdog_telemetry.h) -- WolwifiPhase (16) in particular would
    // directly explain a resend cycle that started and then never
    // continued, no wol-resend-confirmed/gave-up, no error, nothing.
    BoardWatchdogReboot = 17,
    // Appended exactly once, unconditionally, on EVERY boot (the very
    // first trace event, before BoardWatchdogReboot if that also fires) --
    // unlike BoardWatchdogReboot, this doesn't require
    // watchdog_enable_caused_reboot() to be true, so it also catches a
    // brownout, a manual power cycle, a picotool/BOOTSEL reset, or any
    // other reset path the watchdog-specific check wouldn't attribute to
    // itself. detail is the raw watchdog_hw->reason bits (bit0=TIMER,
    // bit1=FORCE, 0=power-on/pin/other non-watchdog reset) -- gives an
    // unambiguous "why did the trace sequence restart at 1" answer instead
    // of inferring it from board_time_ms looking small.
    BoardBoot = 18,
    // A controller-connect edge arrived within WOL_TRIGGER_DEBOUNCE_MS of
    // the last magic packet actually sent -- skipped to avoid re-running
    // Wi-Fi connect/resend (and re-contending with BT) on every edge of a
    // flapping BT link during a single PC boot. detail = seconds since the
    // last send (capped to 255).
    WolTriggerDebounced = 19,
    // Wi-Fi connect retries for the current triggered attempt hit
    // MAX_WIFI_CONNECT_RETRIES (or two consecutive CYW43_LINK_BADAUTH
    // results) -- wolwifi_task()'s Idle case stops auto-reconnecting until
    // the next legitimate trigger (fresh controller-connect edge past the
    // debounce window, or new SSID/password). detail = the retry count (or
    // the raw BADAUTH status value) at the point it gave up.
    WolConnectRetriesExhausted = 20,
    // Added after a real smoke test showed WOL firing ~61 real seconds after
    // wol-trigger-fired with zero trace events in between (no timeout, no
    // retries-exhausted, no ring-buffer drop) despite connect_attempts
    // jumping by 2 in that window -- the existing trace coverage couldn't
    // explain it. These four cover every remaining silent WifiState
    // transition so a repeat has nothing left to infer.
    //
    // Appended on every start_wifi_connect() call, unconditionally (not just
    // on failure/timeout like the existing Wol*Timeout stages) -- detail is
    // g_connect_attempt_count at that call, capped to 255.
    WolConnectStarted = 21,
    // Appended when WifiState::Connected detects the link/IP lease is gone
    // and falls back to Failed -- previously only DS5_LOG'd, no trace
    // event, so a live/established connection dropping (as opposed to a
    // Connecting-phase timeout, which does have WolWifiAssocTimeout) was
    // invisible in a host-off gap. detail = g_link_lost_count, capped.
    WolWifiLinkLostAfterConnect = 22,
    // Appended when WifiState::WaitingForIp successfully reaches Connected
    // -- previously only DS5_LOG'd. detail = 1 if g_send_pending was true at
    // that instant (a resend cycle actually started), 0 if false (connected
    // successfully but nothing was queued to send) -- directly answers
    // whether a given successful connect did anything WOL-relevant.
    WolWifiConnected = 23,
    // Appended when WifiState::Failed's WIFI_RETRY_BACKOFF_MS elapses and it
    // re-enters Idle to retry -- makes the Failed -> Idle -> reconnect cycle
    // visible end-to-end (the timeout stages above only cover Failed's
    // entry, not its exit back toward another connect attempt).
    WolWifiBackoffElapsed = 24,
    // A controller-connect edge arrived while usb_host_active() reports the
    // target PC already on (USB enumerated and not suspended) -- WOL is
    // skipped entirely (no Wi-Fi connect, no resend cycle, no lightbar
    // pulse), since the PC doesn't need waking. See WOL_ALWAYS in
    // CMakeLists.txt for the build-time escape hatch on boards where this
    // heuristic can't work (USB stays active in S5, or Modern Standby).
    WolTriggerSkippedHostActive = 25,
    // A host-alive-gated test showed wol-trigger-fired firing right after
    // board-boot detail=1 (raw watchdog_hw->reason bit0=TIMER set), but
    // BoardWatchdogReboot never appeared -- watchdog_enable_caused_reboot()
    // only returns true when THIS firmware's own watchdog_enable() call
    // (the periodic 1000ms one in main.cpp) caused the reboot; it returns
    // false for a reboot via a direct watchdog_reboot() call elsewhere
    // (there are four such call sites in bt.cpp's disconnect/ACL-pending
    // recovery paths -- "bounded transport recovery" reboots, deliberate,
    // not a stall), even though the raw reason bits are set. That made a
    // real, distinct class of self-triggered reboot invisible in the
    // trace. Appended at all four watchdog_reboot() call sites, before the
    // call, so it's the last event before the ring buffer resets on the
    // new boot. detail: 0=disconnect-retry-exhausted (bt.cpp
    // service_disconnect_recovery), 1=incoming-ACL-pending-timeout,
    // 2=ACL-cancel-did-not-complete.
    BoardTransportRecoveryReboot = 26,
    // Appended each time service_disconnect_recovery() actually sends a
    // disconnect retry (bt.cpp), so the escalation leading up to a
    // BoardTransportRecoveryReboot(detail=0) is visible, not just the
    // reboot itself. detail = disconnect_retry_attempts after this send
    // (1..DISCONNECT_RETRY_MAX_ATTEMPTS).
    ConnDisconnectRetrySent = 27,
    // Measurement-only: a real test showed the host-alive gate
    // (WolTriggerSkippedHostActive) checking usb_host_active() at the
    // exact instant of the controller-connect edge, when this firmware's
    // own USB persona for that session hasn't mounted yet -- usb_mounted
    // only becomes true after usb_handle_controller_transport_ready()
    // (usb.cpp), itself gated on this controller-type-identification
    // handshake, which only starts once wolwifi_on_controller_connect()
    // has already run (same Ready-transition block, bt.cpp). So the gate
    // was checking a signal that structurally cannot be true yet,
    // independent of whether the target PC is actually on. Appended at
    // both controller-type-identification success sites, right after
    // usb_handle_controller_transport_ready() is called, with detail =
    // elapsed ms since connection_phase entered Ready (capped to 255) --
    // real timing data needed to design a bounded observation window
    // (matching awalol/DS5Dongle#207's and DevFreezing/DS5Dongle-WoL's
    // Observe state) as the actual fix, instead of guessing a value. No
    // behavior change from this stage alone.
    ConnControllerTypeIdentified = 28,
    // A real test with the companion app open and the target PC confirmed
    // on still showed WOL firing (wol-trigger-fired) instead of the
    // observation window correctly aborting it -- the window elapsed
    // without ever reading a sustained-active host, even though the host
    // genuinely was active. Added to see exactly what the window observed
    // instead of continuing to guess. Appended once when
    // begin_observe_host() arms the window (wolwifi.cpp), detail =
    // usb_host_active_debug_bits() (usb.h) at that instant -- the state
    // right as the window starts.
    ObserveHostBegin = 29,
    // Appended on every rising/falling edge of usb_host_active() sampled
    // during an active observation window (not every tick, to avoid
    // flooding) -- detail = usb_host_active_debug_bits() at the edge, so
    // the full sequence of what the window actually saw is reconstructable
    // from the trace (e.g. "never once read usb_mounted" vs. "mounted but
    // stayed suspended" vs. "flickered active/inactive faster than the
    // sustain threshold").
    ObserveHostSampleEdge = 30,
    // Appended when the window elapses without a sustained-active read
    // (the branch that calls proceed_with_wol_trigger()) -- detail =
    // usb_host_active_debug_bits() at the moment the window gave up, so
    // the very last observed state is on record even if no earlier edge
    // was informative.
    ObserveHostWindowElapsed = 31,
};
// detail's meaning depends on stage: HCI disconnect reason for
// ConnDisconnected, 1/0 for whether WOL was queued-pending vs. sent
// immediately for WolTriggerFired, etc. -- see append_wol_trace_event() call
// sites for the exact meaning at each site.
void bt_append_wol_trace_event(WolTraceStage stage, uint8_t detail = 0);
struct WolTraceReadResult {
    uint8_t record_count;
    uint8_t record_size;
    uint32_t latest_sequence;
    uint16_t dropped_count;
};
// Packs up to as many ring records as fit in [buffer, buffer+capacity) back
// to back, starting from the oldest not-yet-read record, advancing the
// internal read cursor. Call repeatedly (e.g. once per companion poll) to
// drain the ring without re-sending already-read records.
WolTraceReadResult bt_read_wol_trace(uint8_t *buffer, uint16_t capacity);

void bt_set_lightbar_restore_enabled(bool enabled);
void bt_schedule_lightbar_restore(uint32_t delay_ms);
void bt_lightbar_loop();
// WOL send-in-progress lightbar indicator (see decisions.md): pulsing green
// while wolwifi.cpp is resending a magic packet, solid light green for a
// short hold once the target confirms it woke up, then a true restore to
// whatever color was showing before the indicator started. All are no-ops
// if the controller isn't connected (hid_interrupt_cid == 0).
void bt_wol_indicator_begin();
void bt_wol_indicator_confirm();
void bt_wol_indicator_cancel();
void bt_wol_indicator_loop();
void bt_signal_strength_loop();
void bt_inquiry_loop();
void bt_connection_recovery_loop();
void bt_feature_prefetch_loop();
void bt_output_retry_loop();
std::vector<uint8_t> get_feature_data(uint8_t reportId,uint16_t len);
void init_feature();
void set_feature_data(uint8_t reportId, uint8_t const* data,uint16_t len);

#endif //DS5_BRIDGE_BT_H
