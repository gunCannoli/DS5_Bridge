// LOCAL-ONLY: fork patch, not part of upstream's Wake-on-LAN. See Features-Port.md 7b.
#include "wake_on_lan_udp_frame.h"
#include <cstring>

namespace ds5::wake_on_lan {
void magic_udp_frame(const uint8_t *source, const uint8_t *target, uint8_t *frame) {
    constexpr size_t kPayload = 102;                 // 6 x 0xff + 16 x MAC
    constexpr size_t kUdpLength = 8 + kPayload;      // 110
    constexpr size_t kIpLength = 20 + kUdpLength;    // 130
    std::memset(frame, 0, kUdpFrameSize);
    // Ethernet II: broadcast, source, IPv4.
    std::memset(frame, 0xff, 6);
    std::memcpy(frame + 6, source, 6);
    frame[12] = 0x08; frame[13] = 0x00;
    // IPv4 header (no options). Source 0.0.0.0, destination 255.255.255.255.
    uint8_t *ip = frame + 14;
    ip[0] = 0x45;
    ip[2] = uint8_t(kIpLength >> 8); ip[3] = uint8_t(kIpLength);
    ip[8] = 64;   // TTL
    ip[9] = 17;   // UDP
    std::memset(ip + 16, 0xff, 4);
    uint32_t sum = 0;
    for (size_t i = 0; i < 20; i += 2) sum += uint32_t(ip[i]) << 8 | ip[i + 1];
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    const uint16_t checksum = uint16_t(~sum);
    ip[10] = uint8_t(checksum >> 8); ip[11] = uint8_t(checksum);
    // UDP header: port 9 (discard) to port 9, checksum 0 (not used over IPv4).
    uint8_t *udp = ip + 20;
    udp[1] = 9; udp[3] = 9;
    udp[4] = uint8_t(kUdpLength >> 8); udp[5] = uint8_t(kUdpLength);
    // Magic payload.
    uint8_t *payload = udp + 8;
    std::memset(payload, 0xff, 6);
    for (size_t i = 0; i < 16; ++i) std::memcpy(payload + 6 + i * 6, target, 6);
}
}
