// LOCAL-ONLY: fork patch, not part of upstream's Wake-on-LAN. See Features-Port.md 7b.
#pragma once

#include <cstddef>
#include <cstdint>

// Upstream sends the magic packet as a raw Ethernet frame with EtherType 0x0842
// (magic_frame). Some access points and routers do not forward unknown
// EtherTypes from a Wi-Fi client to the wired side, while an ordinary UDP
// broadcast (what a phone app sends) always passes. magic_udp_frame builds that
// UDP broadcast as a finished Ethernet frame, so it still needs no IP stack,
// DHCP or ARP: the source address is 0.0.0.0, as in a DHCP discover.
//
// Layout: Ethernet II (14) + IPv4 (20) + UDP (8) + magic payload (102) = 144.
// Destination ff:ff:ff:ff:ff:ff / 255.255.255.255, UDP port 9.
namespace ds5::wake_on_lan {
inline constexpr size_t kUdpFrameSize = 144;
void magic_udp_frame(const uint8_t *source, const uint8_t *target, uint8_t *frame);
}
