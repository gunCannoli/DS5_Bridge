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
void bt_set_edge_profile_switching_blocked(bool blocked);
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

// ==========================================================================
// DEBUG-ONLY: board-level WOL/connection trace  (branch: debug/wol-boot-trace)
// ==========================================================================
// A small RAM ring buffer recording BT connection-phase transitions,
// disconnect reasons/timeouts, wolwifi.cpp events, USB power-management /
// descriptor-topology transitions, and periodic full-state snapshots -- so a
// WOL attempt (or a controller drop) that happens while the target PC (and
// therefore the companion app) is off can still be reconstructed after the
// fact, once the app reconnects. The live WOL debug log and the firmware
// UART log both require a companion connection at the moment the event
// happens, which by definition isn't available for a PC-off wake.
//
// This whole block was stripped in a09323b once WOL shipped, then restored +
// extended on the debug/wol-boot-trace branch to diagnose "controller no
// longer survives the boot sequence" after the v1.7.1 merge / the
// keep-wake-receiver-online USB rework. Reverting commits on that branch as a
// range removes it cleanly again.
//
// WolTraceStage is APPEND-ONLY, never renumbered -- old trace records from
// prior firmware must keep decoding to the same names. Removed stages are
// left as unused numbers. Stages 0-31 are the original set; 32+ are the
// debug/wol-boot-trace additions.
enum class WolTraceStage : uint8_t {
    ConnPhaseConnecting = 0,
    ConnPhaseSecuring = 1,
    ConnPhaseHidOpening = 2,
    ConnPhaseReady = 3,
    ConnPhaseDisconnecting = 4,
    ConnSecurityTimeout = 5,
    ConnHidOpeningTimeout = 6,
    ConnHidInterruptFollowupTimeout = 7,
    ConnDisconnected = 8,            // detail = HCI disconnect reason
    WolTriggerFired = 9,
    WolTriggerSkipped = 10,
    // 11 was WolConnectDelayStart (removed -- no pre-delay any more).
    WolResendBegin = 12,
    WolResendConfirmed = 13,
    WolResendGaveUp = 14,
    WolWifiAssocTimeout = 15,        // detail = elapsed ms in phase, capped 255
    WolDhcpWaitTimeout = 16,         // detail = elapsed ms in phase, capped 255
    BoardWatchdogReboot = 17,        // detail = stuck WatchdogMainLoopPhase
    BoardBoot = 18,                  // detail = raw watchdog_hw->reason bits
    WolTriggerDebounced = 19,        // detail = seconds since last send, capped
    WolConnectRetriesExhausted = 20, // detail = retry count / raw BADAUTH status
    WolConnectStarted = 21,          // detail = g_connect_attempt_count, capped
    WolWifiLinkLostAfterConnect = 22,// detail = g_link_lost_count, capped
    WolWifiConnected = 23,           // detail = 1 if g_send_pending else 0
    WolWifiBackoffElapsed = 24,
    WolTriggerSkippedHostActive = 25,
    BoardTransportRecoveryReboot = 26, // detail: 0=disc-retry-exhausted,
                                       // 1=incoming-ACL-pending-timeout,
                                       // 2=ACL-cancel-did-not-complete
    ConnDisconnectRetrySent = 27,      // detail = disconnect_retry_attempts
    ConnControllerTypeIdentified = 28, // detail = ms since Ready, capped 255
    ObserveHostBegin = 29,            // detail = usb_host_active_debug_bits() low byte
    ObserveHostSampleEdge = 30,       // detail = usb_host_active_debug_bits() low byte
    ObserveHostWindowElapsed = 31,    // detail = usb_host_active_debug_bits() low byte

    // ---- debug/wol-boot-trace additions (32+) --------------------------
    // Cover the surfaces the original trace predates: the
    // keep-wake-receiver-online USB descriptor-topology state machine
    // (usb.cpp usb_pm_poll / usb_begin_descriptor_reconnect), the
    // firmware-level idle-disconnect path, the wolwifi_wake_in_progress()
    // suppression gate, and a periodic everything-at-once snapshot so we are
    // never blind between discrete events.

    // A bridge-only <-> full-persona descriptor topology reconnect began
    // (usb_begin_descriptor_reconnect / usb_schedule_topology_reconnect).
    // This does a real tud_disconnect() + dcd_edpt_close_all() +
    // DCD_EVENT_UNPLUGGED, i.e. it tears down every USB endpoint -- a strong
    // candidate for the mid-boot controller drop (radio/stack churn while the
    // BT session is freshest). detail bit0 = target is bridge_only,
    // bit1 = was bridge_only before, bit2 = dcd_edpt_close_all path taken.
    UsbTopologyReconnectBegin = 32,
    // tud_connect() presenting a topology (usb_connect_transport()).
    // detail bit0 = bridge_only.
    UsbTransportConnect = 33,
    // arm_suspend_disconnect() armed the debounced controller power-off.
    // detail bit0 = armed from tud_umount_cb, bit1 = from tud_suspend_cb,
    // bit2 = from another path.
    UsbSuspendArmed = 34,
    // The bt_power_off_controller() branch in usb_pm_poll() was reached.
    // detail bit0 = wolwifi_wake_in_progress() true (so power-off SUPPRESSED,
    // not executed), bit1 = usb_host_suspended, bit2 = !usb_mounted.
    UsbControllerPowerOff = 35,
    // tud_mount_cb / tud_umount_cb fired. detail = usb_host_active_debug_bits()
    // low byte. Sub-stage in the high bit of detail is not enough room, so
    // mount vs umount is two values:
    UsbMount = 36,
    UsbUmount = 37,
    // The firmware idle-disconnect timeout in bt.cpp actually fired a
    // BtControllerDisconnectIntentIdleTimeout disconnect. detail bit0 =
    // audio_output_route_protected() at the time.
    BtIdleDisconnectFired = 38,
    // wolwifi_wake_in_progress() changed value (sampled each wolwifi_task
    // tick). detail bit0 = new value, bit1 = g_resend_active,
    // bit2 = g_send_pending, bit3 = g_wifi_leave_pending,
    // bit4 = g_observe_host_active.
    WolWakeInProgressEdge = 39,
    // Periodic full-state snapshot. Emitted as a WIDE record (see
    // kWolSnapshotRecordSize) rather than the 8-byte event slot -- the
    // companion tells the two apart by the record_size field. Written every
    // ~1s while any WOL/boot activity flag is set, every ~5s otherwise, so
    // even a completely missed discrete event still leaves periodic ground
    // truth. Payload layout: see bt.cpp WolSnapshot / build_wol_trace().
    WolStateSnapshot = 40,
};
// detail's meaning depends on stage -- see the append_wol_trace_event() /
// bt_append_wol_trace_event() call sites for the exact meaning at each site.
void bt_append_wol_trace_event(WolTraceStage stage, uint8_t detail = 0);

// Wide periodic-snapshot record. wolwifi.cpp fills the Wi-Fi-side fields and
// usb_debug_bits (it can see usb_host_active_debug_bits()); bt_append_wol_
// snapshot() fills connection_phase / hid_link_up / wol_indicator_phase /
// board_time_ms itself (those are static in bt.cpp) and frames it into the
// snapshot ring.
#pragma pack(push, 1)
struct WolSnapshot {
    uint8_t  wifi_state;              // WifiState enum
    uint32_t ms_in_wifi_state;
    uint32_t connect_attempt_count;
    int8_t   raw_link_status;        // cyw43_tcpip_link_status()
    uint16_t wifi_join_state;        // cyw43_state.wifi_join_state
    uint8_t  dhcp_state;
    uint8_t  dhcp_tries;
    uint8_t  have_ip;                // 0/1
    uint8_t  wol_guard_bits;         // bit0 intentionally_idle, bit1 retries_exhausted,
                                     // bit2 send_pending, bit3 resend_active,
                                     // bit4 wifi_leave_pending, bit5 observe_host_active,
                                     // bit6 wake_in_progress, bit7 target_confirmed_awake
    uint16_t usb_debug_bits;         // usb_host_active_debug_bits()
    // ---- filled by bt_append_wol_snapshot() ----
    uint8_t  connection_phase;       // BtConnectionPhase
    uint8_t  hid_link_up;            // hid_interrupt_cid != 0
    uint8_t  wol_indicator_phase;    // WolIndicatorPhase
    uint32_t board_time_ms;
};
#pragma pack(pop)
// Caller fills every field EXCEPT the four "filled by" ones above.
void bt_append_wol_snapshot(WolSnapshot snap);

struct WolTraceReadResult {
    uint8_t record_count;
    uint8_t record_size;             // 8 for event records, sizeof(framed
                                     // snapshot) for snapshot records -- a
                                     // single read returns only one kind
    uint32_t latest_sequence;
    uint16_t dropped_count;
    uint8_t is_snapshot;             // 1 if the records in this read are
                                     // WolSnapshot payloads, 0 for events
};
// Drains the EVENT ring: packs up to as many 8-byte event records as fit in
// [buffer, buffer+capacity), oldest-unread first, advancing the read cursor.
WolTraceReadResult bt_read_wol_trace(uint8_t *buffer, uint16_t capacity);
// Drains the SNAPSHOT ring the same way, one wide record per slot.
WolTraceReadResult bt_read_wol_snapshots(uint8_t *buffer, uint16_t capacity);

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
