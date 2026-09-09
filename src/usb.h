//
// Created by awalol on 2026/3/4.
// Modified for DS5 Bridge companion firmware and app integration.
//

#ifndef DS5_BRIDGE_USB_H
#define DS5_BRIDGE_USB_H

#define DEFAULT_COMPANION_SPEAKER_GAIN 1.0f

extern uint8_t mute[2]; // 0: speaker/LED fallback, 1: mic/idle-disconnect fallback
extern float volume[2]; // 0: companion speaker gain, 1: haptics gain
extern uint8_t usb_host_volume_percent[3]; // Speaker, mic, raw line capture.
extern uint8_t usb_host_mute[3]; // Speaker, mic, raw line capture.
extern uint32_t usb_host_volume_set_count[3];
extern float usb_host_speaker_gain; // Host UAC speaker volume as linear gain.

void usb_device_stack_init_disconnected();
uint8_t usb_hid_polling_rate_mode();
bool usb_set_hid_polling_rate_mode(uint8_t mode);
void usb_request_reconnect();
void usb_note_hid_output();
bool usb_host_hid_output_recent();
void usb_pm_poll();
void usb_set_suspend_disconnect_enabled(bool enabled);
bool usb_suspend_disconnect_enabled();
bool usb_host_suspended_active();
bool usb_host_active();
// DEBUG-ONLY (debug/wol-boot-trace): raw bitmask of every flag
// usb_host_active() depends on, PLUS the keep-wake-receiver-online
// descriptor-topology state that landed after the original trace was
// stripped. usb_host_active() alone can't say *why* it read a given value
// (never mounted this session vs. mounted-but-suspended vs. bridge-only
// topology enumerated vs. a topology reconnect in flight).
//   bit0  usb_mounted
//   bit1  tud_inited()
//   bit2  tud_inited() && tud_suspended()
//   bit3  usb_host_suspended        (suspend-debounce-armed flag)
//   bit4  usb_controller_transport_ready
//   bit5  usb_controller_transport_attached
//   bit6  usb_attached_bridge_only
//   bit7  usb_reconnect_target_bridge_only
//   bit8  usb_reconnect_connect_pending
//   bit9  usb_reconnect_requested
//   bit10 usb_controller_transport_transition_pending
//   bit11 usb_suspend_at_us != 0    (power-off debounce armed)
//   bit12 usb_suspend_disconnect    (setting)
//   bit13 usb_wake_on_connect       (setting)
//   bit14 usb_remote_wakeup_armed
uint16_t usb_host_active_debug_bits();
bool usb_speaker_streaming_active();
bool usb_mic_streaming_active();
bool usb_line_streaming_active();
bool usb_mounted_active();
void usb_handle_controller_transport_disconnect(bool expected_disconnect = false);
void usb_handle_controller_link_connected();
void usb_handle_controller_transport_ready();
void usb_set_wake_on_connect(bool enabled);
bool usb_wake_on_connect_enabled();

#endif //DS5_BRIDGE_USB_H
