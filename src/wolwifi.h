// Wake-on-LAN over Wi-Fi for DS5 Bridge.
//
// Sends a standard UDP magic packet to a configured target PC when a
// DualSense controller connects, so the PC wakes over the network. Isolated
// from the Bluetooth/controller code: bt.cpp only calls
// wolwifi_on_controller_connect() at the point a connection becomes Ready
// (see decisions.md for why that call site was chosen). Everything else --
// Wi-Fi connection, retry, and the magic packet itself -- lives here.
//
// Compiled only when ENABLE_WOLWIFI is set (Waveshare RP2350B-Plus-W build).
// On boards without it, the stubs below are no-ops so callers never need an
// #ifdef.
#pragma once
#include <cstdint>
#include <cstddef>

// Debug-status types are declared unconditionally (cheap; just enums/a
// struct) so wolwifi_debug_status()'s signature is identical whether or not
// ENABLE_WOLWIFI is set -- callers never need an #ifdef around the type.
enum class WolDebugAction : uint8_t {
    None = 0,
    Ping = 1,
    SendWol = 2,
};

enum class WolDebugResult : uint8_t {
    Pending = 0,
    Success = 1,
    Timeout = 2,
    NoWifi = 3,
    SendFailed = 4,
    NotConfigured = 5,
};

enum class WolWifiLinkState : uint8_t {
    Unconfigured = 0,
    Idle = 1,
    Connecting = 2,
    WaitingForIp = 3,
    Connected = 4,
    Failed = 5,
};

struct WolDebugStatus {
    WolWifiLinkState link_state;
    WolDebugAction last_action;
    WolDebugResult last_result;
    uint32_t last_action_started_ms;
    // Dotted-quad octets in display order (ip_octets[0] is the first number
    // shown, e.g. the "192" in "192.168.1.5"), all zero if no IP lease.
    // Raw octets rather than a packed uint32_t so there's no ambiguity
    // between lwIP's network-byte-order ip4_addr and this wire format's
    // little-endian write_u32 -- see decisions.md.
    uint8_t ip_octets[4];
    // Raw config-reached-firmware flags. Exposed so a stuck link_state
    // (e.g. permanently Idle) can be diagnosed from the debug status alone
    // -- Idle with wol_enabled=false means wolwifi_task() is intentionally
    // not progressing (see the early-return at the top of wolwifi_task()),
    // not a connect failure.
    bool wol_enabled;
    bool have_ssid;
    bool have_target_mac;
    uint32_t now_ms;              // firmware's current tick, for age math
    uint32_t link_state_entered_ms;
    // Raw cyw43_tcpip_link_status() return value (CYW43_LINK_DOWN/JOIN/NOIP/
    // UP/FAIL/NONET/BADAUTH -- see cyw43.h; range is -3..3, fits in int8_t).
    // NOTE: this is cyw43_tcpip_link_status(), not cyw43_wifi_link_status()
    // -- the latter never returns NOIP/UP at all (it only reflects the
    // low-level wifi_join_state, with no DHCP/IP awareness), which was a
    // real bug here: wolwifi_task() polled cyw43_wifi_link_status() and
    // could never see CYW43_LINK_UP even once DHCP had a bound lease, so
    // it always eventually timed out and restarted the connect from
    // scratch. Exposed because our own WolWifiLinkState collapses several
    // distinct CYW43 states into "Connecting", which isn't enough to tell
    // a flapping auth failure from a slow DHCP lease from a weak signal.
    int8_t raw_link_status;
    // lwIP's own DHCP client state machine (struct dhcp.state, see
    // lwip/prot/dhcp.h DHCP_STATE_*: 0=off, 1=requesting, 2=init,
    // 6=selecting, 10=bound, etc.) and retry count (struct dhcp.tries).
    // This is what caught the cyw43_wifi_link_status() bug above: dhcp_state
    // showed "bound" (a real IP lease) while raw_link_status stayed "join"
    // and our WifiState kept cycling connecting -> failed -> connecting.
    uint8_t dhcp_state;
    uint8_t dhcp_tries;
    // CYW43 driver's raw internal join-state bitmask (cyw43_state.wifi_join_state
    // -- see WIFI_JOIN_STATE_* in cyw43_ctrl.c: bits for ACTIVE/FAIL/NONET/
    // BADAUTH/AUTH/LINK/KEYED). raw_link_status collapses this to one of 7
    // values; the raw bitmask shows exactly which sub-step of association
    // completed, which mattered for diagnosing a reconnect that failed
    // immediately after a working connection dropped (see decisions.md --
    // cyw43_arch_wifi_connect_async() doesn't clean up a prior association
    // itself; a stale join_state on retry was the suspected cause).
    uint16_t wifi_join_state;
    // Lifetime counters, not reset between connect attempts, so the log
    // shows a running history instead of only the most recent event.
    uint32_t connect_attempt_count;   // every time start_wifi_connect() runs
    uint32_t wifi_connect_timeout_count; // Connecting state timed out without CYW43_LINK_UP
    uint32_t dhcp_timeout_count;      // WaitingForIp state timed out without an IP lease
    uint32_t link_lost_count;         // Connected state detected link/IP loss
};

#ifdef ENABLE_WOLWIFI

// Must be called once during boot, after cyw43_arch_init() (main.cpp already
// calls cyw43_arch_init() for Bluetooth; WOL reuses that same CYW43 radio).
void wolwifi_init(void);

// Polled every main-loop iteration. Non-blocking: drives Wi-Fi
// connect/reconnect and any pending magic-packet send. Never blocks on
// network I/O regardless of Wi-Fi state.
void wolwifi_task(void);

// Call exactly once per controller connect edge (DISCONNECTED -> CONNECTED).
// No-ops if WOL is disabled, unconfigured, or Wi-Fi isn't connected --
// controller operation must never depend on this succeeding.
void wolwifi_on_controller_connect(void);

// Runtime configuration, applied by the companion app. All setters are
// safe to call at any time; they take effect on the next wolwifi_task().
void wolwifi_set_enabled(bool enabled);
bool wolwifi_is_enabled(void);

// SSID and password are set independently (the companion app sends them as
// separate commands). len excludes any NUL terminator. Returns false (and
// leaves the prior value untouched) if len exceeds the firmware's max, or
// data is null with nonzero len -- firmware never trusts the companion
// app's client-side validation alone.
bool wolwifi_set_wifi_ssid(const char *ssid, uint8_t ssid_len);
bool wolwifi_set_wifi_password(const char *password, uint8_t password_len);

// mac must be exactly 6 raw bytes (not a "AA:BB:.." string). Returns false
// (leaving prior config untouched) on a null pointer.
bool wolwifi_set_target_mac(const uint8_t mac[6]);

// Debug/smoke-test entry points, triggered on demand from the companion
// app's WOL debug row (see decisions.md). Both are fire-and-forget: they
// start (or restart) an async operation and return immediately; poll
// wolwifi_debug_status() for the result.
//
// Ping: broadcasts an ARP request, then watches inbound ARP traffic for a
// reply/announcement whose source MAC matches the configured target --
// lwIP's ARP API only resolves IP->MAC, not the reverse, so this snoops
// the netif's raw input rather than using etharp_query/etharp_request
// directly. Confirms the target's NIC is on the network and answering ARP,
// which is what actually matters for a WOL target (its NIC listens for
// magic packets even with the OS off/asleep).
void wolwifi_debug_ping(void);

// Re-sends the magic packet right now (bypassing the controller-connect
// trigger), regardless of Wi-Fi/target-MAC state -- the result reports
// exactly why if it can't.
void wolwifi_debug_send_wol(void);

WolDebugStatus wolwifi_debug_status(void);

#else

static inline void wolwifi_init(void) {}
static inline void wolwifi_task(void) {}
static inline void wolwifi_on_controller_connect(void) {}
static inline void wolwifi_set_enabled(bool) {}
static inline bool wolwifi_is_enabled(void) { return false; }
static inline bool wolwifi_set_wifi_ssid(const char *, uint8_t) { return false; }
static inline bool wolwifi_set_wifi_password(const char *, uint8_t) { return false; }
static inline bool wolwifi_set_target_mac(const uint8_t[6]) { return false; }
static inline void wolwifi_debug_ping(void) {}
static inline void wolwifi_debug_send_wol(void) {}
static inline WolDebugStatus wolwifi_debug_status(void) {
    WolDebugStatus status{};
    status.link_state = WolWifiLinkState::Unconfigured;
    status.last_action = WolDebugAction::None;
    status.last_result = WolDebugResult::NotConfigured;
    return status;
}

#endif // ENABLE_WOLWIFI
