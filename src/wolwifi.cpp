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

#include <cstring>

#include "pico/cyw43_arch.h"
#include "lwip/udp.h"
#include "lwip/dhcp.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "netif/ethernet.h"

#include "utils.h"

namespace {

constexpr uint16_t WOL_UDP_PORT = 9;              // standard WOL discard-port target
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
constexpr uint32_t WIFI_RETRY_BACKOFF_MS = 10000;
constexpr uint32_t DEBUG_PING_TIMEOUT_MS = 4000;
constexpr uint8_t MAX_SSID_LEN = 32;               // 802.11 SSID max
// WPA2-PSK passphrases can be up to 63 chars, but the companion protocol's
// fixed 63-byte HID report (11-byte command header) only has 53 bytes of
// payload room -- see WOL_WIFI_PASSWORD_MAX_LENGTH in shared/protocol.ts.
constexpr uint8_t MAX_PASSWORD_LEN = 53;
constexpr uint16_t ETHTYPE_ARP = 0x0806;
constexpr std::size_t ETH_HEADER_LEN = 14;
constexpr std::size_t ETH_SRC_MAC_OFFSET = 6;

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

udp_pcb *g_udp_pcb = nullptr;
bool g_send_pending = false;

WolDebugAction g_debug_action = WolDebugAction::None;
WolDebugResult g_debug_result = WolDebugResult::Pending;
uint32_t g_debug_started_ms = 0;
bool g_arp_snoop_installed = false;
netif_input_fn g_original_netif_input = nullptr;

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
// every inbound Ethernet frame's source MAC for an ARP ping in progress,
// then always forwards to the netif's original input function (normally
// ethernet_input) so nothing about ordinary lwIP operation changes.
err_t arp_snoop_input(pbuf *p, netif *inp) {
    if (
        g_debug_action == WolDebugAction::Ping
        && g_debug_result == WolDebugResult::Pending
        && g_have_target_mac
        && p->len >= ETH_HEADER_LEN
    ) {
        uint8_t src_mac[6];
        if (pbuf_copy_partial(p, src_mac, sizeof(src_mac), ETH_SRC_MAC_OFFSET) == sizeof(src_mac)) {
            if (std::memcmp(src_mac, g_target_mac, sizeof(src_mac)) == 0) {
                finish_debug_action(WolDebugResult::Success);
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
    DS5_LOG("[WOL] Wi-Fi connecting to \"%s\"\n", g_ssid);
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

} // namespace

void wolwifi_init(void) {
    g_udp_pcb = udp_new();
    enter_state(WifiState::Unconfigured);
    DS5_LOG("[WOL] Initialized\n");
}

void wolwifi_set_enabled(bool enabled) {
    g_enabled = enabled;
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

    DS5_LOG("[WOL] Wi-Fi SSID updated (len=%u)\n", ssid_len);

    // Re-apply on next task poll rather than blocking here.
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

    DS5_LOG("[WOL] Wi-Fi password updated (len=%u)\n", password_len);

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
    DS5_LOG(
        "[WOL] Target MAC set to %02X:%02X:%02X:%02X:%02X:%02X\n",
        g_target_mac[0], g_target_mac[1], g_target_mac[2],
        g_target_mac[3], g_target_mac[4], g_target_mac[5]
    );
    return true;
}

void wolwifi_on_controller_connect(void) {
    if (!g_enabled || !g_have_ssid || !g_have_target_mac) {
        return;
    }
    DS5_LOG("[WOL] Controller connected\n");
    if (g_wifi_state == WifiState::Connected && have_ip_lease()) {
        send_magic_packet_now(false);
    } else {
        // Not connected yet: queue the send for once wolwifi_task() gets us
        // online. wolwifi_task() will start/retry the Wi-Fi connect itself.
        g_send_pending = true;
        DS5_LOG("[WOL] Wi-Fi not ready; queuing magic packet\n");
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
    return status;
}

void wolwifi_task(void) {
    if (
        g_debug_action == WolDebugAction::Ping
        && g_debug_result == WolDebugResult::Pending
        && now_ms() - g_debug_started_ms > DEBUG_PING_TIMEOUT_MS
    ) {
        finish_debug_action(WolDebugResult::Timeout);
    }

    if (!g_enabled || !g_have_ssid) {
        return;
    }

    switch (g_wifi_state) {
        case WifiState::Unconfigured:
            return;

        case WifiState::Idle:
            start_wifi_connect();
            return;

        case WifiState::Connecting: {
            const int status = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);
            if (status == CYW43_LINK_UP) {
                DS5_LOG("[WOL] Wi-Fi link up; waiting for IP lease\n");
                enter_state(WifiState::WaitingForIp);
            } else if (status < 0 || now_ms() - g_state_entered_ms > WIFI_CONNECT_TIMEOUT_MS) {
                DS5_LOG("[WOL] Wi-Fi connect failed/timed out (status=%d)\n", status);
                enter_state(WifiState::Failed);
            }
            return;
        }

        case WifiState::WaitingForIp: {
            if (have_ip_lease()) {
                const ip4_addr_t *addr = netif_ip4_addr(netif_default);
                DS5_LOG(
                    "[WOL] Wi-Fi connected: %s\n",
                    ip4addr_ntoa(addr)
                );
                enter_state(WifiState::Connected);
                ensure_arp_snoop_installed();
                if (g_send_pending) {
                    g_send_pending = false;
                    send_magic_packet_now(false);
                }
            } else if (now_ms() - g_state_entered_ms > WIFI_CONNECT_TIMEOUT_MS) {
                DS5_LOG("[WOL] Timed out waiting for DHCP lease\n");
                enter_state(WifiState::Failed);
            }
            return;
        }

        case WifiState::Connected: {
            const int status = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);
            if (status != CYW43_LINK_UP || !have_ip_lease()) {
                DS5_LOG("[WOL] Wi-Fi link lost; will retry\n");
                enter_state(WifiState::Failed);
            }
            return;
        }

        case WifiState::Failed:
            if (now_ms() - g_state_entered_ms > WIFI_RETRY_BACKOFF_MS) {
                enter_state(WifiState::Idle);
            }
            return;
    }
}

#endif // ENABLE_WOLWIFI
