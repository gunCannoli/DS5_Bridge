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
#include "wolwifi.h"

#ifdef ENABLE_WOLWIFI

#include <cstring>

#include "pico/cyw43_arch.h"
#include "lwip/udp.h"
#include "lwip/dhcp.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"

#include "utils.h"

namespace {

constexpr uint16_t WOL_UDP_PORT = 9;              // standard WOL discard-port target
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
constexpr uint32_t WIFI_RETRY_BACKOFF_MS = 10000;
constexpr uint8_t MAX_SSID_LEN = 32;               // 802.11 SSID max
constexpr uint8_t MAX_PASSWORD_LEN = 63;           // WPA2-PSK passphrase max

enum class WifiState : uint8_t {
    Unconfigured,
    Idle,
    Connecting,
    WaitingForIp,
    Connected,
    Failed,
};

volatile bool g_enabled = false;
bool g_have_credentials = false;
bool g_have_target_mac = false;

char g_ssid[MAX_SSID_LEN + 1] = {0};
char g_password[MAX_PASSWORD_LEN + 1] = {0};
uint8_t g_target_mac[6] = {0};

WifiState g_wifi_state = WifiState::Unconfigured;
uint32_t g_state_entered_ms = 0;

udp_pcb *g_udp_pcb = nullptr;
bool g_send_pending = false;

uint32_t now_ms() {
    return to_ms_since_boot(get_absolute_time());
}

void enter_state(WifiState s) {
    g_wifi_state = s;
    g_state_entered_ms = now_ms();
}

bool build_magic_packet(uint8_t out[102], const uint8_t mac[6]) {
    std::memset(out, 0xFF, 6);
    for (int i = 0; i < 16; ++i) {
        std::memcpy(out + 6 + i * 6, mac, 6);
    }
    return true;
}

void send_magic_packet_now() {
    if (g_udp_pcb == nullptr || !g_have_target_mac) {
        return;
    }
    uint8_t payload[102];
    build_magic_packet(payload, g_target_mac);

    pbuf *p = pbuf_alloc(PBUF_TRANSPORT, sizeof(payload), PBUF_RAM);
    if (p == nullptr) {
        DS5_LOG("[WOL] Magic packet alloc failed\n");
        return;
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
    } else {
        DS5_LOG("[WOL] Magic packet send failed (err=%d)\n", static_cast<int>(err));
    }
}

bool have_ip_lease() {
    struct netif *n = netif_default;
    return n != nullptr && !ip4_addr_isany_val(*netif_ip4_addr(n));
}

void start_wifi_connect() {
    if (!g_have_credentials) {
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

bool wolwifi_set_wifi_credentials(const char *ssid, uint8_t ssid_len,
                                   const char *password, uint8_t password_len) {
    if (ssid_len > MAX_SSID_LEN || password_len > MAX_PASSWORD_LEN) {
        DS5_LOG("[WOL] Rejected Wi-Fi credentials: length out of range\n");
        return false;
    }
    if ((ssid_len > 0 && ssid == nullptr) || (password_len > 0 && password == nullptr)) {
        DS5_LOG("[WOL] Rejected Wi-Fi credentials: null buffer\n");
        return false;
    }

    std::memset(g_ssid, 0, sizeof(g_ssid));
    std::memset(g_password, 0, sizeof(g_password));
    if (ssid_len > 0) {
        std::memcpy(g_ssid, ssid, ssid_len);
    }
    if (password_len > 0) {
        std::memcpy(g_password, password, password_len);
    }
    g_have_credentials = ssid_len > 0;

    DS5_LOG("[WOL] Wi-Fi credentials updated (ssid_len=%u)\n", ssid_len);

    // Re-apply on next task poll rather than blocking here.
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
    if (!g_enabled || !g_have_credentials || !g_have_target_mac) {
        return;
    }
    DS5_LOG("[WOL] Controller connected\n");
    if (g_wifi_state == WifiState::Connected && have_ip_lease()) {
        send_magic_packet_now();
    } else {
        // Not connected yet: queue the send for once wolwifi_task() gets us
        // online. wolwifi_task() will start/retry the Wi-Fi connect itself.
        g_send_pending = true;
        DS5_LOG("[WOL] Wi-Fi not ready; queuing magic packet\n");
    }
}

void wolwifi_task(void) {
    if (!g_enabled || !g_have_credentials) {
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
                if (g_send_pending) {
                    g_send_pending = false;
                    send_magic_packet_now();
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
