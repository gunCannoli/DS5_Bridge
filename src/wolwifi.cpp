// Wake-on-LAN over Wi-Fi for DS5 Bridge. See wolwifi.h for the module
// contract and decisions.md for why this is isolated the way it is.
//
// Mechanism: cyw43_arch_wifi_connect_async() joins the configured SSID in
// the background; wolwifi_task() polls the link/DHCP state each main-loop
// iteration and, once an IP lease is bound, sends a queued magic packet
// with lwIP's raw (NO_SYS=1) UDP API. Nothing here blocks -- a stalled or
// absent Wi-Fi network just leaves the packet unsent; the controller and
// Bluetooth are never affected (see decisions.md: this module owns Wi-Fi,
// bt.cpp only fires the trigger).
//
// Debug ping/send (wolwifi_debug_*) let the companion app exercise this
// path on demand while the target PC is on, since that PC and the one the
// board's USB hub is plugged into can be the same machine -- once WOL
// actually needs to fire (PC off), there is no PC left to run the
// companion app and read logs from. See decisions.md.
#include "wolwifi.h"

#ifdef ENABLE_WOLWIFI

#include <algorithm>
#include <cstring>

#include "pico/cyw43_arch.h"
#include "lwip/udp.h"
#include "lwip/dhcp.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "netif/ethernet.h"

#include "btstack_tlv.h"

#include "bt.h"
#include "utils.h"

namespace {

constexpr uint16_t WOL_UDP_PORT = 9;              // standard WOL discard-port target
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
// Association itself (join to the AP, CYW43_LINK_JOIN/NOIP) was observed
// to complete quickly, but DHCP frequently stalled for the full 15s
// WIFI_CONNECT_TIMEOUT_MS before timing out and retrying -- a likely
// feedback loop with the BT/Wi-Fi radio-contention issue (DHCP slow
// because BT is contending with it, BT drops because DHCP keeps retrying
// for so long). A separate, shorter timeout specifically for the
// WaitingForIp (DHCP) phase caps how long any single attempt can keep
// contending with BT, at the cost of occasionally abandoning a DHCP
// attempt that would have succeeded a little later -- the retry backoff
// below picks it back up.
constexpr uint32_t DHCP_WAIT_TIMEOUT_MS = 3000;
constexpr uint32_t WIFI_RETRY_BACKOFF_MS = 10000;
constexpr uint32_t DEBUG_PING_TIMEOUT_MS = 4000;
// CYW43439 is a combo Wi-Fi/Bluetooth chip that time-shares one radio, so
// a Wi-Fi STA association + DHCP handshake right at controller-connect
// does contend with the still-fresh BT session. An earlier fix delayed
// the Wi-Fi connect start by 2s to let BT settle first, but per-user
// requirement WOL must fire the instant the controller connects (matching
// awalol/DS5Dongle#207, which starts its Wi-Fi connect immediately with
// no pre-delay) -- a delayed, best-effort wake isn't acceptable even if
// it's occasionally more radio-contention-safe. The controller is instead
// protected from the resulting contention by wolwifi_wake_in_progress()
// (bug 6: suppresses the USB-suspend controller power-off for the whole
// attempt) and by BT's own reconnect behavior tolerating a drop -- not by
// avoiding the contention window in the first place.
// A single UDP magic packet has no delivery guarantee, and the first send
// can land during the brief post-connect reconnect flap seen in testing
// (see decisions.md, first successful WOL Test) and simply be lost.
// Resend periodically until the target confirms it's awake (via the same
// ARP-snoop liveness check the debug Ping button uses) or this budget runs
// out, rather than firing once and hoping.
constexpr uint32_t WOL_RESEND_INTERVAL_MS = 3000;
constexpr uint32_t WOL_RESEND_TOTAL_BUDGET_MS = 15000;
// Real board-trace data (see decisions.md, "Thirteenth bug diagnosed")
// showed HCI 0x22 (LMP/LL response timeout) disconnects landing within
// ~0.3-5s of wol-resend-confirmed specifically (not wol-resend-gave-up) --
// traced to disconnect_wifi_after_wol()'s cyw43_wifi_leave() call and the
// lightbar's bt_wol_indicator_confirm() (two BT sends bracketing a 2s hold,
// see WOL_INDICATOR_CONFIRMED_HOLD_MS in bt.cpp) both firing back to back
// in the same tick, stacking two radio-contention bursts right when the
// controller most needs to survive (the PC is still booting).
// cyw43_wifi_leave() -> cyw43_ioctl(CYW43_IOCTL_SET_DISASSOC) is a
// synchronous ioctl that kicks off an async over-the-air deauth/cleanup
// exchange continuing to use the shared radio after the call returns (see
// cyw43_ctrl.c in the Pico SDK). Deferring the actual leave past both the
// lightbar's 2s hold and the resend cycle's own 3s send interval separates
// the two bursts instead of stacking them, without touching the lightbar
// code (bt_wol_indicator_confirm()/_cancel() still fire immediately -- only
// the underlying disassociation is delayed).
constexpr uint32_t WOL_DISCONNECT_DELAY_MS = 3000;
// A flapping BT link during a single PC boot can fire multiple
// controller-connect edges in quick succession (several of the bugs in
// decisions.md were this exact scenario), and without a guard every one of
// those re-enters wolwifi_on_controller_connect() and restarts Wi-Fi/WOL
// activity, re-contending with BT each time for no benefit -- one magic
// packet getting through is enough. Debounced by when a packet was last
// actually *sent* (not merely triggered), matching
// DevFreezing/DS5Dongle-WoL's wol.cpp DEBOUNCE_US design: an aborted/failed
// attempt shouldn't consume the window, so a fresh connect can still retry
// soon if nothing went out last time.
constexpr uint32_t WOL_TRIGGER_DEBOUNCE_MS = 90000;
// CYW43_LINK_BADAUTH (wrong Wi-Fi password) can never succeed no matter how
// many times it's retried -- without a cap it's otherwise indistinguishable
// from a transient "AP briefly unreachable" failure and gets the same
// infinite WIFI_RETRY_BACKOFF_MS retry loop, needless radio activity/BT
// contention for a connect that will never work. Matches
// DevFreezing/DS5Dongle-WoL's wol.cpp MAX_RETRIES (2), which also gives a
// first BADAUTH one retry (covers a genuine transient PSK handshake
// glitch) before giving up.
constexpr uint8_t MAX_WIFI_CONNECT_RETRIES = 2;
constexpr uint8_t MAX_SSID_LEN = 32;               // 802.11 SSID max
// WPA2-PSK passphrases can be up to 63 chars, but the companion protocol's
// fixed 63-byte HID report (11-byte command header) only has 53 bytes of
// payload room -- see WOL_WIFI_PASSWORD_MAX_LENGTH in shared/protocol.ts.
constexpr uint8_t MAX_PASSWORD_LEN = 53;
constexpr uint16_t ETHTYPE_ARP = 0x0806;
constexpr std::size_t ETH_HEADER_LEN = 14;
constexpr std::size_t ETH_SRC_MAC_OFFSET = 6;
constexpr std::size_t ETH_TYPE_OFFSET = 12;

enum class WifiState : uint8_t {
    Unconfigured,
    Idle,
    Connecting,
    WaitingForIp,
    Connected,
    Failed,
};

volatile bool g_enabled = false;
bool g_have_ssid = false;
bool g_have_target_mac = false;

char g_ssid[MAX_SSID_LEN + 1] = {0};
char g_password[MAX_PASSWORD_LEN + 1] = {0};
uint8_t g_target_mac[6] = {0};

WifiState g_wifi_state = WifiState::Unconfigured;
uint32_t g_state_entered_ms = 0;
uint32_t g_connect_attempt_count = 0;
uint32_t g_link_lost_count = 0;
uint32_t g_dhcp_timeout_count = 0;
uint32_t g_wifi_connect_timeout_count = 0;

udp_pcb *g_udp_pcb = nullptr;
bool g_send_pending = false;
// Set by disconnect_wifi_after_wol() after a resend cycle ends; stops
// wolwifi_task()'s Idle case from immediately reconnecting Wi-Fi on its
// very next tick (the Idle state is otherwise "connect whenever possible"
// by design, which -- without this guard -- turned the intentional
// post-WOL disconnect into a self-inflicted immediate leave/rejoin loop,
// defeating the whole point of leaving: the ongoing DHCP/association
// activity from the rejoin was still observed contending with BT and
// dropping the controller mid-boot even with the leave call in place.
// Cleared only by wolwifi_on_controller_connect(), the one legitimate
// reason to reconnect.
bool g_wifi_intentionally_idle = false;

// See WOL_TRIGGER_DEBOUNCE_MS. 0 = never sent (debounce never blocks the
// very first attempt). Set only when a send actually succeeds, in
// send_magic_packet_now() -- not on trigger, not on a failed/aborted
// attempt.
uint32_t g_last_wol_sent_ms = 0;

// See MAX_WIFI_CONNECT_RETRIES. Reset to 0 at the start of every fresh
// triggered attempt (wolwifi_on_controller_connect(), post-debounce), so
// the cap applies per attempt cycle, not cumulatively across the board's
// whole uptime. g_badauth_seen tracks whether the *current* run of
// consecutive failures included a BADAUTH, mirroring the reference
// implementation's one-retry-then-stop BADAUTH handling distinct from the
// generic retry-until-MAX_WIFI_CONNECT_RETRIES path.
uint8_t g_connect_retry_count = 0;
bool g_badauth_seen = false;
// Set once retries are exhausted (BADAUTH twice, or the generic cap hit) so
// WifiState::Idle stops auto-reconnecting for this attempt cycle -- same
// guard shape as g_wifi_intentionally_idle, cleared by the same legitimate
// reconnect triggers (fresh controller-connect edge, new SSID/password).
bool g_wifi_retries_exhausted = false;

WolDebugAction g_debug_action = WolDebugAction::None;
WolDebugResult g_debug_result = WolDebugResult::Pending;
uint32_t g_debug_started_ms = 0;
bool g_arp_snoop_installed = false;
netif_input_fn g_original_netif_input = nullptr;

// Automatic-trigger resend tracking (see WOL_RESEND_INTERVAL_MS above).
// Not used by the debug WOL Test button, which is a single fire-and-forget
// send. g_resend_active also gates the liveness ARP watch in
// arp_snoop_input() below -- separate from the debug Ping's own watch, so
// the two can't clobber each other's result if run back to back.
bool g_resend_active = false;
uint32_t g_resend_started_ms = 0;
uint32_t g_resend_last_sent_ms = 0;
bool g_target_confirmed_awake = false;

// See WOL_DISCONNECT_DELAY_MS. Armed by drive_resend_cycle()'s
// confirmed/gave-up branches instead of calling disconnect_wifi_after_wol()
// synchronously, so the actual cyw43_wifi_leave() radio activity doesn't
// stack with the lightbar indicator's own BT sends in the same tick.
// Checked every wolwifi_task() tick, independent of g_wifi_state (same
// reasoning as drive_resend_cycle() itself). Cleared (without firing) by a
// fresh wolwifi_on_controller_connect() trigger -- a new attempt has its
// own reason to keep Wi-Fi up and doesn't want a stale leave firing
// mid-attempt.
bool g_wifi_leave_pending = false;
uint32_t g_wifi_leave_not_before_ms = 0;

uint32_t now_ms() {
    return to_ms_since_boot(get_absolute_time());
}

void enter_state(WifiState s) {
    g_wifi_state = s;
    g_state_entered_ms = now_ms();
}

WolWifiLinkState public_link_state() {
    switch (g_wifi_state) {
        case WifiState::Unconfigured: return WolWifiLinkState::Unconfigured;
        case WifiState::Idle: return WolWifiLinkState::Idle;
        case WifiState::Connecting: return WolWifiLinkState::Connecting;
        case WifiState::WaitingForIp: return WolWifiLinkState::WaitingForIp;
        case WifiState::Connected: return WolWifiLinkState::Connected;
        case WifiState::Failed: return WolWifiLinkState::Failed;
    }
    return WolWifiLinkState::Unconfigured;
}

void start_debug_action(WolDebugAction action) {
    g_debug_action = action;
    g_debug_result = WolDebugResult::Pending;
    g_debug_started_ms = now_ms();
}

void finish_debug_action(WolDebugResult result) {
    g_debug_result = result;
    DS5_LOG(
        "[WOL] Debug action %s finished: result=%u\n",
        g_debug_action == WolDebugAction::Ping ? "ping" : "send-wol",
        static_cast<unsigned>(result)
    );
}

bool build_magic_packet(uint8_t out[102], const uint8_t mac[6]) {
    std::memset(out, 0xFF, 6);
    for (int i = 0; i < 16; ++i) {
        std::memcpy(out + 6 + i * 6, mac, 6);
    }
    return true;
}

// Returns true (and reports the debug result) on failure, so callers can
// early-return; only the "actually sent" DS5_LOG path is shared with the
// non-debug trigger.
bool send_magic_packet_now(bool is_debug_action) {
    if (g_udp_pcb == nullptr || !g_have_target_mac) {
        if (is_debug_action) {
            finish_debug_action(WolDebugResult::NotConfigured);
        }
        return false;
    }
    uint8_t payload[102];
    build_magic_packet(payload, g_target_mac);

    pbuf *p = pbuf_alloc(PBUF_TRANSPORT, sizeof(payload), PBUF_RAM);
    if (p == nullptr) {
        DS5_LOG("[WOL] Magic packet alloc failed\n");
        if (is_debug_action) {
            finish_debug_action(WolDebugResult::SendFailed);
        }
        return false;
    }
    std::memcpy(p->payload, payload, sizeof(payload));

    ip_addr_t broadcast_addr;
    ip_addr_set_any(false, &broadcast_addr);
    IP4_ADDR(ip_2_ip4(&broadcast_addr), 255, 255, 255, 255);

    const err_t err = udp_sendto(g_udp_pcb, p, &broadcast_addr, WOL_UDP_PORT);
    pbuf_free(p);

    if (err == ERR_OK) {
        DS5_LOG(
            "[WOL] Sent magic packet to %02X:%02X:%02X:%02X:%02X:%02X\n",
            g_target_mac[0], g_target_mac[1], g_target_mac[2],
            g_target_mac[3], g_target_mac[4], g_target_mac[5]
        );
        // Arms WOL_TRIGGER_DEBOUNCE_MS from the moment a packet actually
        // went out -- not from when the trigger fired -- so a failed/
        // aborted attempt doesn't consume the debounce window. Only the
        // automatic trigger's sends should debounce future automatic
        // triggers; the debug WOL Test button is on-demand tooling and
        // must keep working every time it's pressed regardless of the
        // automatic path's timing.
        if (!is_debug_action) {
            g_last_wol_sent_ms = now_ms();
        }
        if (is_debug_action) {
            finish_debug_action(WolDebugResult::Success);
        }
        return true;
    }

    DS5_LOG("[WOL] Magic packet send failed (err=%d)\n", static_cast<int>(err));
    if (is_debug_action) {
        finish_debug_action(WolDebugResult::SendFailed);
    }
    return false;
}

bool have_ip_lease() {
    struct netif *n = netif_default;
    return n != nullptr && !ip4_addr_isany_val(*netif_ip4_addr(n));
}

// Installed as netif_default->input once Wi-Fi first connects. Inspects
// every inbound ARP frame's source MAC for an ARP ping in progress, then
// always forwards to the netif's original input function (normally
// ethernet_input) so nothing about ordinary lwIP operation changes.
//
// EtherType is checked (ETHTYPE_ARP) before matching the source MAC --
// originally this matched on *any* Ethernet frame's source address, ARP
// or not. That's a real false-positive risk: any non-ARP broadcast/
// multicast traffic that happens to carry the target's MAC as source
// (switch/AP-level relaying, proxy behavior, etc.) would satisfy the
// match and report "confirmed awake" even with the target genuinely
// still off. Confirmed as a live bug: real-world traces showed
// wol-resend-confirmed firing within 1-3s of wol-resend-begin on every
// cycle, far too fast to be an actual PC finishing a cold boot and
// answering ARP, immediately followed by the resend cycle ending and
// Wi-Fi disconnecting (bug 9's disconnect_wifi_after_wol()) -- so the
// real magic packet resend window was being cut short by a false
// confirmation, before the target's NIC had actually had a chance to
// wake the machine and come back on the network for real.
err_t arp_snoop_input(pbuf *p, netif *inp) {
    const bool debug_ping_watching =
        g_debug_action == WolDebugAction::Ping && g_debug_result == WolDebugResult::Pending;
    if (
        (debug_ping_watching || g_resend_active)
        && g_have_target_mac
        && p->len >= ETH_HEADER_LEN
    ) {
        uint8_t eth_type[2];
        if (
            pbuf_copy_partial(p, eth_type, sizeof(eth_type), ETH_TYPE_OFFSET) == sizeof(eth_type)
            && (static_cast<uint16_t>(eth_type[0]) << 8 | eth_type[1]) == ETHTYPE_ARP
        ) {
            uint8_t src_mac[6];
            if (pbuf_copy_partial(p, src_mac, sizeof(src_mac), ETH_SRC_MAC_OFFSET) == sizeof(src_mac)) {
                if (std::memcmp(src_mac, g_target_mac, sizeof(src_mac)) == 0) {
                    if (debug_ping_watching) {
                        finish_debug_action(WolDebugResult::Success);
                    }
                    if (g_resend_active) {
                        g_target_confirmed_awake = true;
                    }
                }
            }
        }
    }
    if (g_original_netif_input != nullptr) {
        return g_original_netif_input(p, inp);
    }
    return ethernet_input(p, inp);
}

void ensure_arp_snoop_installed() {
    if (g_arp_snoop_installed || netif_default == nullptr) {
        return;
    }
    g_original_netif_input = netif_default->input;
    netif_default->input = arp_snoop_input;
    g_arp_snoop_installed = true;
}

void start_wifi_connect() {
    if (!g_have_ssid) {
        enter_state(WifiState::Unconfigured);
        return;
    }
    g_connect_attempt_count++;
    // cyw43_arch_wifi_connect_async() -> cyw43_wifi_join() does not clean up
    // a prior association itself (the SDK docs say cyw43_wifi_leave() is a
    // separate, caller-managed step) -- confirmed via wifi_join_state that a
    // retry after a genuine link loss was joining on top of stale driver
    // state and failing immediately (raw_link_status=fail) where the first
    // connect from a clean boot succeeded. Explicitly leave first on every
    // attempt after the first so retries start from a clean join state.
    if (g_connect_attempt_count > 1) {
        const int leave_err = cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
        DS5_LOG(
            "[WOL] Wi-Fi leave before reconnect attempt #%u: err=%d, prior join_state=0x%04x\n",
            g_connect_attempt_count, leave_err, cyw43_state.wifi_join_state
        );
    }
    DS5_LOG(
        "[WOL] Wi-Fi connecting to \"%s\" (attempt #%u)\n",
        g_ssid, g_connect_attempt_count
    );
    bt_append_wol_trace_event(
        WolTraceStage::WolConnectStarted,
        static_cast<uint8_t>(std::min<uint32_t>(g_connect_attempt_count, 255))
    );
    cyw43_arch_enable_sta_mode();
    const int err = cyw43_arch_wifi_connect_async(
        g_ssid, g_password, CYW43_AUTH_WPA2_AES_PSK
    );
    if (err) {
        DS5_LOG("[WOL] Wi-Fi connect request failed (err=%d)\n", err);
        enter_state(WifiState::Failed);
        return;
    }
    enter_state(WifiState::Connecting);
}

// Persisted WOL config: saved to on-board flash (via BTstack's TLV store,
// the same mechanism already used for pairing keys and the controller
// blacklist -- see bt.cpp bt_blacklist_persist()/_load()) so the config
// survives a reboot without depending on the companion app to re-send it.
//
// Why this exists: the companion app only re-applies settings after it
// reconnects post-boot, as one of the last steps in a long sequential
// commands list (applyCurrentSettings() in bridge-service.ts). A paired
// controller's own BT reconnect can complete well before that finishes --
// confirmed via the board-level WOL trace, which showed
// wol-trigger-skipped with all of enabled/have-ssid/have-target-mac still
// false at the moment connection_phase reached Ready, both times, on a
// fresh boot. Loading from flash in wolwifi_init() (called right after
// bt_init() starts, before BT can possibly finish a reconnect) closes that
// race entirely -- WOL is armed from the moment the board powers on,
// independent of whether a companion app ever talks to it that session.
constexpr uint32_t WOL_CONFIG_TLV_TAG = 0x574f4c43u; // ASCII 'WOLC'

#pragma pack(push, 1)
struct WolPersistedConfig {
    uint8_t version;
    uint8_t enabled;
    uint8_t ssid_len;
    char ssid[MAX_SSID_LEN];
    uint8_t password_len;
    char password[MAX_PASSWORD_LEN];
    uint8_t have_target_mac;
    uint8_t target_mac[6];
};
#pragma pack(pop)
constexpr uint8_t WOL_CONFIG_VERSION = 1;

void wolwifi_persist_config() {
    const btstack_tlv_t *tlv = nullptr;
    void *tlv_context = nullptr;
    btstack_tlv_get_instance(&tlv, &tlv_context);
    if (tlv == nullptr) {
        DS5_LOG("[WOL] No TLV instance available, not persisting config\n");
        return;
    }

    WolPersistedConfig config{};
    config.version = WOL_CONFIG_VERSION;
    config.enabled = g_enabled ? 1 : 0;
    config.ssid_len = g_have_ssid ? static_cast<uint8_t>(std::strlen(g_ssid)) : 0;
    std::memcpy(config.ssid, g_ssid, sizeof(config.ssid));
    config.password_len = static_cast<uint8_t>(std::strlen(g_password));
    std::memcpy(config.password, g_password, sizeof(config.password));
    config.have_target_mac = g_have_target_mac ? 1 : 0;
    std::memcpy(config.target_mac, g_target_mac, sizeof(config.target_mac));

    const int rc = tlv->store_tag(
        tlv_context,
        WOL_CONFIG_TLV_TAG,
        reinterpret_cast<const uint8_t *>(&config),
        sizeof(config)
    );
    DS5_LOG("[WOL] Persisted config to flash (rc=%d)\n", rc);
}

void wolwifi_load_persisted_config() {
    const btstack_tlv_t *tlv = nullptr;
    void *tlv_context = nullptr;
    btstack_tlv_get_instance(&tlv, &tlv_context);
    if (tlv == nullptr) {
        DS5_LOG("[WOL] No TLV instance available, config stays unconfigured\n");
        return;
    }

    WolPersistedConfig config{};
    const int len = tlv->get_tag(
        tlv_context,
        WOL_CONFIG_TLV_TAG,
        reinterpret_cast<uint8_t *>(&config),
        sizeof(config)
    );
    if (len != static_cast<int>(sizeof(config)) || config.version != WOL_CONFIG_VERSION) {
        DS5_LOG("[WOL] No persisted config found (len=%d)\n", len);
        return;
    }

    g_enabled = config.enabled != 0;
    if (config.ssid_len > 0 && config.ssid_len <= MAX_SSID_LEN) {
        std::memcpy(g_ssid, config.ssid, config.ssid_len);
        g_ssid[config.ssid_len] = '\0';
        g_have_ssid = true;
    }
    if (config.password_len > 0 && config.password_len <= MAX_PASSWORD_LEN) {
        std::memcpy(g_password, config.password, config.password_len);
        g_password[config.password_len] = '\0';
    }
    if (config.have_target_mac) {
        std::memcpy(g_target_mac, config.target_mac, sizeof(g_target_mac));
        g_have_target_mac = true;
    }
    DS5_LOG(
        "[WOL] Loaded persisted config: enabled=%d have_ssid=%d have_target_mac=%d\n",
        g_enabled ? 1 : 0, g_have_ssid ? 1 : 0, g_have_target_mac ? 1 : 0
    );
}

} // namespace

void wolwifi_init(void) {
    g_udp_pcb = udp_new();
    wolwifi_load_persisted_config();
    // wolwifi_task()'s Unconfigured case is a dead end -- it never
    // re-checks g_have_ssid, so if wolwifi_load_persisted_config() just
    // loaded a valid SSID from flash, staying in Unconfigured would leave
    // the state machine stuck until something else (only the SSID/password
    // setters call enter_state(Idle)) nudges it out. Without this, a fresh
    // boot's persisted config is loaded correctly but Wi-Fi genuinely
    // cannot start connecting until the companion app's slow post-connect
    // settings-reapply sequence happens to re-send the SSID -- the same
    // race the flash-persistence fix closed for the config *values*, still
    // open for the state machine itself. See decisions.md.
    enter_state(g_have_ssid ? WifiState::Idle : WifiState::Unconfigured);
    DS5_LOG("[WOL] Initialized\n");
}

void wolwifi_set_enabled(bool enabled) {
    g_enabled = enabled;
    wolwifi_persist_config();
    DS5_LOG("[WOL] %s\n", enabled ? "Enabled" : "Disabled");
}

bool wolwifi_is_enabled(void) {
    return g_enabled;
}

bool wolwifi_set_wifi_ssid(const char *ssid, uint8_t ssid_len) {
    if (ssid_len > MAX_SSID_LEN) {
        DS5_LOG("[WOL] Rejected Wi-Fi SSID: length out of range\n");
        return false;
    }
    if (ssid_len > 0 && ssid == nullptr) {
        DS5_LOG("[WOL] Rejected Wi-Fi SSID: null buffer\n");
        return false;
    }

    std::memset(g_ssid, 0, sizeof(g_ssid));
    if (ssid_len > 0) {
        std::memcpy(g_ssid, ssid, ssid_len);
    }
    g_have_ssid = ssid_len > 0;
    wolwifi_persist_config();

    DS5_LOG("[WOL] Wi-Fi SSID updated (len=%u)\n", ssid_len);

    // Re-apply on next task poll rather than blocking here. New credentials
    // are a legitimate reason to reconnect even if a prior WOL cycle left
    // Wi-Fi intentionally idle.
    g_wifi_intentionally_idle = false;
    enter_state(WifiState::Idle);
    return true;
}

bool wolwifi_set_wifi_password(const char *password, uint8_t password_len) {
    if (password_len > MAX_PASSWORD_LEN) {
        DS5_LOG("[WOL] Rejected Wi-Fi password: length out of range\n");
        return false;
    }
    if (password_len > 0 && password == nullptr) {
        DS5_LOG("[WOL] Rejected Wi-Fi password: null buffer\n");
        return false;
    }

    std::memset(g_password, 0, sizeof(g_password));
    if (password_len > 0) {
        std::memcpy(g_password, password, password_len);
    }
    wolwifi_persist_config();

    DS5_LOG("[WOL] Wi-Fi password updated (len=%u)\n", password_len);

    g_wifi_intentionally_idle = false;
    enter_state(WifiState::Idle);
    return true;
}

bool wolwifi_set_target_mac(const uint8_t mac[6]) {
    if (mac == nullptr) {
        DS5_LOG("[WOL] Rejected target MAC: null buffer\n");
        return false;
    }
    std::memcpy(g_target_mac, mac, 6);
    g_have_target_mac = true;
    wolwifi_persist_config();
    DS5_LOG(
        "[WOL] Target MAC set to %02X:%02X:%02X:%02X:%02X:%02X\n",
        g_target_mac[0], g_target_mac[1], g_target_mac[2],
        g_target_mac[3], g_target_mac[4], g_target_mac[5]
    );
    return true;
}

// Drops the Wi-Fi association once a resend cycle ends (confirmed or
// timed out). WOL doesn't need Wi-Fi connected once the magic-packet
// situation is resolved for this controller session, and staying
// associated (DHCP renewal, beacon listening, general STA housekeeping)
// is ongoing CYW43 radio activity that was observed causing a Bluetooth
// LMP/LL response timeout (HCI disconnect reason 0x22) partway through a
// real PC boot, well after the initial connect-time contention the 2s
// connect-start delay already covers -- that fix only protects the start
// of the Wi-Fi session, not its whole duration. The next controller-
// connect trigger reconnects Wi-Fi fresh via the existing delayed-start +
// leave-before-rejoin path, so this doesn't cost anything beyond the
// normal reconnect latency next time WOL is needed.
void disconnect_wifi_after_wol() {
    if (g_wifi_state != WifiState::Connected) {
        return;
    }
    const int err = cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    DS5_LOG("[WOL] Wi-Fi leave after resend cycle end: err=%d\n", err);
    g_wifi_intentionally_idle = true;
    enter_state(WifiState::Idle);
}

// See WOL_DISCONNECT_DELAY_MS: arms a deferred cyw43_wifi_leave() instead
// of calling disconnect_wifi_after_wol() synchronously, so its radio
// activity doesn't stack with the lightbar indicator's own BT sends
// (bt_wol_indicator_confirm()/_cancel(), called right before this in both
// of drive_resend_cycle()'s branches) in the same tick.
void arm_deferred_wifi_leave() {
    g_wifi_leave_pending = true;
    g_wifi_leave_not_before_ms = now_ms() + WOL_DISCONNECT_DELAY_MS;
}

// Starts (or restarts) the resend-until-confirmed cycle: sends immediately,
// then wolwifi_task()'s drive_resend_cycle() takes over to resend every
// WOL_RESEND_INTERVAL_MS until either the target confirms awake via ARP or
// WOL_RESEND_TOTAL_BUDGET_MS elapses.
void begin_resend_cycle() {
    g_resend_active = true;
    g_target_confirmed_awake = false;
    g_resend_started_ms = now_ms();
    g_resend_last_sent_ms = g_resend_started_ms;
    bt_append_wol_trace_event(WolTraceStage::WolResendBegin);
    ensure_arp_snoop_installed();
    bt_wol_indicator_begin();
    send_magic_packet_now(false);
}

// Drives the resend-until-confirmed cycle; called every wolwifi_task() tick.
// No-op unless a cycle is active (started by begin_resend_cycle()).
void drive_resend_cycle() {
    if (!g_resend_active) {
        return;
    }
    if (g_target_confirmed_awake) {
        const uint32_t took_ms = now_ms() - g_resend_started_ms;
        DS5_LOG(
            "[WOL] Target confirmed awake via ARP after %lu ms; stopping resend\n",
            static_cast<unsigned long>(took_ms)
        );
        bt_append_wol_trace_event(
            WolTraceStage::WolResendConfirmed,
            static_cast<uint8_t>(std::min<uint32_t>(took_ms / 1000, 255))
        );
        g_resend_active = false;
        bt_wol_indicator_confirm();
        arm_deferred_wifi_leave();
        return;
    }
    const uint32_t elapsed = now_ms() - g_resend_started_ms;
    if (elapsed > WOL_RESEND_TOTAL_BUDGET_MS) {
        DS5_LOG(
            "[WOL] Resend budget (%lu ms) exhausted without ARP confirmation; giving up\n",
            static_cast<unsigned long>(WOL_RESEND_TOTAL_BUDGET_MS)
        );
        bt_append_wol_trace_event(WolTraceStage::WolResendGaveUp);
        g_resend_active = false;
        bt_wol_indicator_cancel();
        arm_deferred_wifi_leave();
        return;
    }
    if (now_ms() - g_resend_last_sent_ms >= WOL_RESEND_INTERVAL_MS) {
        g_resend_last_sent_ms = now_ms();
        // Broadcast an ARP request too, same as the debug Ping, to prompt a
        // reply from an already-awake target sooner rather than waiting
        // only on ambient ARP chatter.
        if (netif_default != nullptr) {
            etharp_request(netif_default, netif_ip4_addr(netif_default));
        }
        send_magic_packet_now(false);
    }
}

void wolwifi_on_controller_connect(void) {
    if (!g_enabled || !g_have_ssid || !g_have_target_mac) {
        // detail bitmask: bit0=enabled, bit1=have_ssid, bit2=have_target_mac
        // -- lets the trace show *why* a connect-trigger edge did nothing,
        // distinguishing "WOL genuinely off/unconfigured" from "trigger
        // never fired at all" (the latter wouldn't appear in the trace at
        // all, which is itself diagnostic).
        const uint8_t detail = (g_enabled ? 1 : 0) | (g_have_ssid ? 2 : 0) | (g_have_target_mac ? 4 : 0);
        bt_append_wol_trace_event(WolTraceStage::WolTriggerSkipped, detail);
        return;
    }
    // See WOL_TRIGGER_DEBOUNCE_MS: a flapping BT link during a single PC
    // boot can fire this multiple times in quick succession; one magic
    // packet getting through is enough, and re-running Wi-Fi connect/resend
    // on every edge only re-contends with BT for no benefit. Checked before
    // the retry-count reset below so a debounced edge doesn't also reset a
    // still-relevant in-progress attempt's retry budget.
    if (g_last_wol_sent_ms != 0 && now_ms() - g_last_wol_sent_ms < WOL_TRIGGER_DEBOUNCE_MS) {
        const uint32_t since_last_ms = now_ms() - g_last_wol_sent_ms;
        DS5_LOG(
            "[WOL] Controller connected but debounced (last send %lu ms ago, window %lu ms)\n",
            static_cast<unsigned long>(since_last_ms),
            static_cast<unsigned long>(WOL_TRIGGER_DEBOUNCE_MS)
        );
        bt_append_wol_trace_event(
            WolTraceStage::WolTriggerDebounced,
            static_cast<uint8_t>(std::min<uint32_t>(since_last_ms / 1000, 255))
        );
        return;
    }
    DS5_LOG("[WOL] Controller connected\n");
    // A fresh controller-connect edge is the one legitimate reason to
    // reconnect Wi-Fi after a prior WOL cycle intentionally left it idle
    // (see disconnect_wifi_after_wol()) or after a prior attempt exhausted
    // its connect retries (see MAX_WIFI_CONNECT_RETRIES) -- clear both
    // guards and reset the per-attempt retry counters so this fresh attempt
    // gets its own full retry budget. Also cancel any still-pending deferred
    // leave from a just-finished prior attempt (see WOL_DISCONNECT_DELAY_MS)
    // -- this fresh attempt has its own reason to keep Wi-Fi up and doesn't
    // want a stale leave firing mid-attempt.
    g_wifi_intentionally_idle = false;
    g_wifi_retries_exhausted = false;
    g_connect_retry_count = 0;
    g_badauth_seen = false;
    g_wifi_leave_pending = false;
    if (g_wifi_state == WifiState::Connected && have_ip_lease()) {
        // Wi-Fi is already up from a prior session -- no new radio activity
        // about to start, so no reason to delay; begin resending right away.
        bt_append_wol_trace_event(WolTraceStage::WolTriggerFired, 0);
        begin_resend_cycle();
    } else {
        // Not connected yet: queue the send for once wolwifi_task() gets us
        // online, and let the Idle case start the Wi-Fi connect on its very
        // next tick -- no artificial delay. WOL must fire the instant the
        // controller connects (matching awalol/DS5Dongle#207's behavior),
        // not some seconds later.
        g_send_pending = true;
        bt_append_wol_trace_event(WolTraceStage::WolTriggerFired, 1);
        DS5_LOG("[WOL] Wi-Fi not ready; queuing magic packet, starting connect now\n");
    }
}

void wolwifi_debug_ping(void) {
    DS5_LOG("[WOL] Debug ping requested\n");
    start_debug_action(WolDebugAction::Ping);
    if (!g_have_target_mac) {
        finish_debug_action(WolDebugResult::NotConfigured);
        return;
    }
    if (g_wifi_state != WifiState::Connected || !have_ip_lease()) {
        finish_debug_action(WolDebugResult::NoWifi);
        return;
    }
    ensure_arp_snoop_installed();
    // Broadcast an ARP request for our own address (gratuitous-style) to
    // prompt any listening host to update its ARP cache and, incidentally,
    // generate ARP chatter on the segment; the snoop above is what actually
    // detects the target -- this isn't targeted at the target IP since we
    // may not know it, only its MAC.
    etharp_request(netif_default, netif_ip4_addr(netif_default));
}

void wolwifi_debug_send_wol(void) {
    DS5_LOG("[WOL] Debug send requested\n");
    start_debug_action(WolDebugAction::SendWol);
    if (!g_have_target_mac) {
        finish_debug_action(WolDebugResult::NotConfigured);
        return;
    }
    if (g_wifi_state != WifiState::Connected || !have_ip_lease()) {
        finish_debug_action(WolDebugResult::NoWifi);
        return;
    }
    send_magic_packet_now(true);
}

WolDebugStatus wolwifi_debug_status(void) {
    WolDebugStatus status{};
    status.link_state = public_link_state();
    status.last_action = g_debug_action;
    status.last_result = g_debug_result;
    status.last_action_started_ms = g_debug_started_ms;
    if (have_ip_lease()) {
        const ip4_addr_t *addr = netif_ip4_addr(netif_default);
        status.ip_octets[0] = ip4_addr1(addr);
        status.ip_octets[1] = ip4_addr2(addr);
        status.ip_octets[2] = ip4_addr3(addr);
        status.ip_octets[3] = ip4_addr4(addr);
    } else {
        status.ip_octets[0] = status.ip_octets[1] = status.ip_octets[2] = status.ip_octets[3] = 0;
    }
    status.wol_enabled = g_enabled;
    status.have_ssid = g_have_ssid;
    status.have_target_mac = g_have_target_mac;
    status.now_ms = now_ms();
    status.link_state_entered_ms = g_state_entered_ms;
    status.raw_link_status = static_cast<int8_t>(cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA));
    if (netif_default != nullptr) {
        const struct dhcp *dhcp = netif_dhcp_data(netif_default);
        status.dhcp_state = dhcp != nullptr ? dhcp->state : 0;
        status.dhcp_tries = dhcp != nullptr ? dhcp->tries : 0;
    } else {
        status.dhcp_state = 0;
        status.dhcp_tries = 0;
    }
    status.wifi_join_state = cyw43_state.wifi_join_state;
    status.connect_attempt_count = g_connect_attempt_count;
    status.wifi_connect_timeout_count = g_wifi_connect_timeout_count;
    status.dhcp_timeout_count = g_dhcp_timeout_count;
    status.link_lost_count = g_link_lost_count;
    return status;
}

bool wolwifi_wake_in_progress(void) {
    // g_send_pending covers the gap between a controller-connect trigger
    // and the resend cycle actually starting (still waiting on the delayed
    // Wi-Fi connect / DHCP lease) -- the controller needs to stay present
    // through that gap too, not just once resending begins. g_wifi_leave_pending
    // covers the WOL_DISCONNECT_DELAY_MS gap between the resend cycle ending
    // and the deferred Wi-Fi leave actually firing -- without it, the
    // USB-suspend controller-power-off suppression this function drives
    // would have a gap during exactly the window this fix introduces.
    return g_resend_active || g_send_pending || g_wifi_leave_pending;
}

void wolwifi_task(void) {
    if (
        g_debug_action == WolDebugAction::Ping
        && g_debug_result == WolDebugResult::Pending
        && now_ms() - g_debug_started_ms > DEBUG_PING_TIMEOUT_MS
    ) {
        finish_debug_action(WolDebugResult::Timeout);
    }

    // Rate-limited: logs only on the edge into/out of the blocked state,
    // not every poll tick, to avoid flooding the log while WOL is
    // intentionally off/unconfigured.
    static bool was_blocked = false;
    const bool blocked = !g_enabled || !g_have_ssid;
    if (blocked != was_blocked) {
        DS5_LOG(
            "[WOL] Task %s: wol_enabled=%d have_ssid=%d have_target_mac=%d\n",
            blocked ? "blocked" : "unblocked",
            g_enabled ? 1 : 0, g_have_ssid ? 1 : 0, g_have_target_mac ? 1 : 0
        );
        was_blocked = blocked;
    }
    if (blocked) {
        return;
    }

    // Drive any in-progress resend-until-confirmed cycle every tick,
    // independent of g_wifi_state below -- a resend can span a Wi-Fi link
    // drop/reconnect (e.g. the reconnect flap seen in testing), and the
    // cycle's own budget/ARP-confirmation logic is what decides when to
    // stop, not the connect state machine.
    drive_resend_cycle();

    // Fire a deferred Wi-Fi leave armed by drive_resend_cycle() (see
    // WOL_DISCONNECT_DELAY_MS/arm_deferred_wifi_leave()) once its delay has
    // elapsed. Checked every tick, independent of g_wifi_state, same as
    // drive_resend_cycle() above.
    if (g_wifi_leave_pending && now_ms() >= g_wifi_leave_not_before_ms) {
        g_wifi_leave_pending = false;
        disconnect_wifi_after_wol();
    }

    // Log every DHCP client state transition (see lwip/prot/dhcp.h
    // DHCP_STATE_*) regardless of our own WifiState -- useful alongside
    // raw_link_status to confirm DHCP is actually running.
    if (netif_default != nullptr) {
        static uint8_t last_logged_dhcp_state = 0xFF; // sentinel outside DHCP_STATE_* range
        const struct dhcp *dhcp = netif_dhcp_data(netif_default);
        const uint8_t dhcp_state = dhcp != nullptr ? dhcp->state : 0;
        if (dhcp_state != last_logged_dhcp_state) {
            DS5_LOG(
                "[WOL] DHCP client state: %u (tries=%u, has_struct=%d)\n",
                dhcp_state, dhcp != nullptr ? dhcp->tries : 0, dhcp != nullptr ? 1 : 0
            );
            last_logged_dhcp_state = dhcp_state;
        }
    }

    switch (g_wifi_state) {
        case WifiState::Unconfigured:
            return;

        case WifiState::Idle:
            if (g_wifi_intentionally_idle || g_wifi_retries_exhausted) {
                return;
            }
            start_wifi_connect();
            return;

        case WifiState::Connecting: {
            const int status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
            // Log every status transition seen while connecting (not just
            // the terminal ones) -- CYW43_LINK_JOIN/NOIP/UP/FAIL/NONET/
            // BADAUTH are all distinct and a flapping link (associate,
            // get an IP, drop) looks identical to "still connecting" from
            // our WolWifiLinkState alone without this.
            static int last_logged_status = -100; // sentinel outside CYW43_LINK_* range
            if (status != last_logged_status) {
                DS5_LOG("[WOL] Wi-Fi link status: %d\n", status);
                last_logged_status = status;
            }
            if (status == CYW43_LINK_UP) {
                DS5_LOG("[WOL] Wi-Fi link up; waiting for IP lease\n");
                enter_state(WifiState::WaitingForIp);
            } else if (status == CYW43_LINK_BADAUTH) {
                // Usually a wrong password (can never succeed no matter how
                // many times it's retried), but the driver can also report a
                // transient BADAUTH from a PSK handshake glitch on marginal
                // signal, which a rejoin does fix -- give it exactly one
                // retry before treating it as a real auth failure and
                // stopping. See MAX_WIFI_CONNECT_RETRIES.
                if (!g_badauth_seen) {
                    g_badauth_seen = true;
                    DS5_LOG("[WOL] Wi-Fi BADAUTH (may be transient); retrying once\n");
                    enter_state(WifiState::Failed);
                } else {
                    DS5_LOG("[WOL] Wi-Fi BADAUTH again; wrong credentials, giving up\n");
                    bt_append_wol_trace_event(WolTraceStage::WolConnectRetriesExhausted, status & 0xFF);
                    g_wifi_retries_exhausted = true;
                    enter_state(WifiState::Failed);
                }
            } else if (status < 0 || now_ms() - g_state_entered_ms > WIFI_CONNECT_TIMEOUT_MS) {
                g_wifi_connect_timeout_count++;
                const uint32_t elapsed_ms = now_ms() - g_state_entered_ms;
                DS5_LOG(
                    "[WOL] Wi-Fi connect failed/timed out (status=%d, join_state=0x%04x, "
                    "elapsed_ms=%lu, connect_timeout_count=%lu)\n",
                    status, cyw43_state.wifi_join_state,
                    static_cast<unsigned long>(elapsed_ms),
                    static_cast<unsigned long>(g_wifi_connect_timeout_count)
                );
                bt_append_wol_trace_event(
                    WolTraceStage::WolWifiAssocTimeout,
                    static_cast<uint8_t>(std::min<uint32_t>(elapsed_ms / 1000, 255))
                );
                g_connect_retry_count++;
                if (g_connect_retry_count > MAX_WIFI_CONNECT_RETRIES) {
                    DS5_LOG(
                        "[WOL] Wi-Fi connect retries exhausted (%u > max %u); giving up until next trigger\n",
                        g_connect_retry_count, MAX_WIFI_CONNECT_RETRIES
                    );
                    bt_append_wol_trace_event(WolTraceStage::WolConnectRetriesExhausted, g_connect_retry_count);
                    g_wifi_retries_exhausted = true;
                }
                enter_state(WifiState::Failed);
            }
            return;
        }

        case WifiState::WaitingForIp: {
            if (have_ip_lease()) {
                const ip4_addr_t *addr = netif_ip4_addr(netif_default);
                DS5_LOG(
                    "[WOL] Wi-Fi connected: %s (attempt #%lu)\n",
                    ip4addr_ntoa(addr), static_cast<unsigned long>(g_connect_attempt_count)
                );
                bt_append_wol_trace_event(
                    WolTraceStage::WolWifiConnected,
                    g_send_pending ? 1 : 0
                );
                enter_state(WifiState::Connected);
                ensure_arp_snoop_installed();
                if (g_send_pending) {
                    g_send_pending = false;
                    begin_resend_cycle();
                }
            } else if (now_ms() - g_state_entered_ms > DHCP_WAIT_TIMEOUT_MS) {
                g_dhcp_timeout_count++;
                const struct dhcp *dhcp = netif_default != nullptr ? netif_dhcp_data(netif_default) : nullptr;
                const uint32_t elapsed_ms = now_ms() - g_state_entered_ms;
                DS5_LOG(
                    "[WOL] Timed out waiting for DHCP lease (dhcp_state=%u, dhcp_tries=%u, "
                    "join_state=0x%04x, dhcp_timeout_count=%lu)\n",
                    dhcp != nullptr ? dhcp->state : 0, dhcp != nullptr ? dhcp->tries : 0,
                    cyw43_state.wifi_join_state, static_cast<unsigned long>(g_dhcp_timeout_count)
                );
                bt_append_wol_trace_event(
                    WolTraceStage::WolDhcpWaitTimeout,
                    static_cast<uint8_t>(std::min<uint32_t>(elapsed_ms / 1000, 255))
                );
                // A stalled DHCP exchange counts against the same connect
                // retry budget as an association failure (both are "this
                // attempt cycle isn't working") -- see MAX_WIFI_CONNECT_RETRIES.
                g_connect_retry_count++;
                if (g_connect_retry_count > MAX_WIFI_CONNECT_RETRIES) {
                    DS5_LOG(
                        "[WOL] Wi-Fi connect retries exhausted (%u > max %u) after DHCP timeout; "
                        "giving up until next trigger\n",
                        g_connect_retry_count, MAX_WIFI_CONNECT_RETRIES
                    );
                    bt_append_wol_trace_event(WolTraceStage::WolConnectRetriesExhausted, g_connect_retry_count);
                    g_wifi_retries_exhausted = true;
                }
                enter_state(WifiState::Failed);
            }
            return;
        }

        case WifiState::Connected: {
            const int status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
            if (status != CYW43_LINK_UP || !have_ip_lease()) {
                g_link_lost_count++;
                DS5_LOG(
                    "[WOL] Wi-Fi link lost; will retry (status=%d, have_ip=%d, "
                    "join_state=0x%04x, connected_for_ms=%lu, link_lost_count=%lu)\n",
                    status, have_ip_lease() ? 1 : 0, cyw43_state.wifi_join_state,
                    static_cast<unsigned long>(now_ms() - g_state_entered_ms),
                    static_cast<unsigned long>(g_link_lost_count)
                );
                bt_append_wol_trace_event(
                    WolTraceStage::WolWifiLinkLostAfterConnect,
                    static_cast<uint8_t>(std::min<uint32_t>(g_link_lost_count, 255))
                );
                enter_state(WifiState::Failed);
            }
            return;
        }

        case WifiState::Failed:
            if (now_ms() - g_state_entered_ms > WIFI_RETRY_BACKOFF_MS) {
                bt_append_wol_trace_event(WolTraceStage::WolWifiBackoffElapsed);
                enter_state(WifiState::Idle);
            }
            return;
    }
}

#endif // ENABLE_WOLWIFI
