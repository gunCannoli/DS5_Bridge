// LOCAL-ONLY: fork patch test. See Features-Port.md 7b.
#include "wake_on_lan_udp_frame.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace ds5::wake_on_lan;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"UDP frame check failed at line %d: %s\n",__LINE__,#x); std::abort(); } } while (0)

int main() {
    const uint8_t source[] = {2, 4, 6, 8, 10, 12};
    const uint8_t target[] = {0x02, 0x23, 0x45, 0x67, 0x89, 0xab};
    std::array<uint8_t, kUdpFrameSize + 2> buf{};
    buf.front() = 0x5a; buf.back() = 0xa5;  // guard bytes
    uint8_t *f = buf.data() + 1;
    magic_udp_frame(source, target, f);
    CHECK(buf.front() == 0x5a && buf.back() == 0xa5);
    CHECK(kUdpFrameSize == 144);
    // Ethernet II: broadcast, source, IPv4.
    for (unsigned i = 0; i < 6; ++i) CHECK(f[i] == 0xff);
    CHECK(!std::memcmp(f + 6, source, 6));
    CHECK(f[12] == 0x08 && f[13] == 0x00);
    // IPv4: version/IHL, total length 130, TTL, UDP, 0.0.0.0 -> 255.255.255.255.
    const uint8_t *ip = f + 14;
    CHECK(ip[0] == 0x45 && ip[2] == 0 && ip[3] == 130 && ip[9] == 17);
    for (unsigned i = 12; i < 16; ++i) CHECK(ip[i] == 0);
    for (unsigned i = 16; i < 20; ++i) CHECK(ip[i] == 0xff);
    // A header that includes its own checksum sums to 0xffff.
    uint32_t sum = 0;
    for (unsigned i = 0; i < 20; i += 2) sum += uint32_t(ip[i]) << 8 | ip[i + 1];
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    CHECK(sum == 0xffff);
    // UDP: 9 -> 9, length 110, checksum unused.
    const uint8_t *udp = ip + 20;
    CHECK(udp[0] == 0 && udp[1] == 9 && udp[2] == 0 && udp[3] == 9);
    CHECK(udp[4] == 0 && udp[5] == 110 && udp[6] == 0 && udp[7] == 0);
    // Magic payload: 6 x 0xff, then the target MAC 16 times.
    const uint8_t *payload = udp + 8;
    for (unsigned i = 0; i < 6; ++i) CHECK(payload[i] == 0xff);
    for (unsigned i = 0; i < 16; ++i) CHECK(!std::memcmp(payload + 6 + i * 6, target, 6));
    CHECK(payload + 102 == f + kUdpFrameSize);
    std::puts("wake_on_lan_udp_frame_test passed");
    return 0;
}
