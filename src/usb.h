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
// Raw bitmask snapshot of every flag usb_host_active() depends on, plus
// the controller-transport session-scoping state -- see wolwifi.cpp's
// ObserveHost diagnostics (WolTraceStage::ObserveHostSample) for why: a
// real test showed the host-alive gate not skipping WOL even with the
// companion app open and the target PC on, and usb_host_active() alone
// can't tell you *why* it read false (never mounted this session vs.
// mounted-but-suspended vs. controller-transport not yet attached).
// bit0=usb_mounted, bit1=tud_inited(), bit2=tud_suspended(),
// bit3=usb_host_suspended (the suspend-debounce-armed flag, distinct from
// the live tud_suspended() bus state), bit4=usb_controller_transport_ready,
// bit5=usb_controller_transport_attached.
uint8_t usb_host_active_debug_bits();
bool usb_speaker_streaming_active();
bool usb_mic_streaming_active();
bool usb_line_streaming_active();
void usb_handle_controller_transport_disconnect(bool expected_disconnect = false);
void usb_handle_controller_transport_ready();
void usb_wake_host_if_suspended();
void usb_set_wake_on_connect(bool enabled);
bool usb_wake_on_connect_enabled();
bool usb_controller_transport_retained_for_wake();

#endif //DS5_BRIDGE_USB_H
