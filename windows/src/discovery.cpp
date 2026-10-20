#include "discovery.h"

#include "net.h"

#include <bcrypt.h>
#include <iphlpapi.h>

namespace {

constexpr uint16_t kDiscoveryPort = 7401;
constexpr uint32_t kGroup = (239u << 24) | (255u << 16) | (77u << 8) | 87u;  // 239.255.77.87
constexpr char kRequest[] = "WSDISC2";
constexpr char kReply[] = "WSHERE2";

void SendProbe(SOCKET s, uint32_t addressHostOrder, const std::vector<uint8_t>& request) {
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(kDiscoveryPort);
    target.sin_addr.s_addr = htonl(addressHostOrder);
    sendto(s, reinterpret_cast<const char*>(request.data()), static_cast<int>(request.size()), 0,
           reinterpret_cast<const sockaddr*>(&target), sizeof target);
}

}  // namespace

std::vector<DiscoveredHost> DiscoverHosts(const std::string& token, const std::vector<std::wstring>& knownHosts, int timeoutMs) {
    std::vector<DiscoveredHost> found;

    uint8_t nonce[16];
    BCryptGenRandom(nullptr, nonce, sizeof nonce, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    std::vector<uint8_t> request(kRequest, kRequest + 7);
    request.insert(request.end(), nonce, nonce + sizeof nonce);
    const std::string context = "windowsstream-discover";
    std::vector<uint8_t> proofInput(context.begin(), context.end());
    proofInput.insert(proofInput.end(), nonce, nonce + sizeof nonce);
    uint8_t expected[32];
    if (!HmacSha256(token, proofInput.data(), proofInput.size(), expected)) return found;

    ULONG size = 32 * 1024;
    std::vector<uint8_t> buffer(size);
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG status = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    if (status == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        status = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (status != NO_ERROR) return found;

    // 每个网卡地址一个 UDP 套接字：分别向本网段广播地址和 255.255.255.255 发送探测。
    std::vector<SOCKET> sockets;
    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
            auto* local = reinterpret_cast<sockaddr_in*>(unicast->Address.lpSockaddr);
            if (local->sin_family != AF_INET) continue;
            uint32_t ip = ntohl(local->sin_addr.s_addr);
            int prefix = unicast->OnLinkPrefixLength;
            if ((ip >> 24) == 127 || prefix <= 0 || prefix >= 31) continue;
            uint32_t mask = 0xFFFFFFFFu << (32 - prefix);
            uint32_t broadcast = (ip & mask) | ~mask;

            SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (s == INVALID_SOCKET) continue;
            BOOL on = TRUE;
            setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof on);
            sockaddr_in bindAddress = *local;
            bindAddress.sin_port = 0;
            if (bind(s, reinterpret_cast<const sockaddr*>(&bindAddress), sizeof bindAddress) != 0) {
                closesocket(s);
                continue;
            }
            in_addr multicastInterface = local->sin_addr;
            setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&multicastInterface), sizeof multicastInterface);
            DWORD ttl = 1;
            setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof ttl);
            SendProbe(s, kGroup, request);
            SendProbe(s, broadcast, request);
            SendProbe(s, 0xFFFFFFFFu, request);
            sockets.push_back(s);
        }
    }

    // 单播询问已知地址（不依赖广播/组播，最可靠）。
    if (SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); s != INVALID_SOCKET) {
        for (const auto& host : knownHosts) {
            in_addr address{};
            if (InetPtonW(AF_INET, host.c_str(), &address) == 1) {
                SendProbe(s, ntohl(address.s_addr), request);
            }
        }
        sockets.push_back(s);
    }

    uint64_t start = NowMicros();
    uint64_t deadline = start + static_cast<uint64_t>(timeoutMs) * 1000;
    while (!sockets.empty()) {
        uint64_t now = NowMicros();
        if (now >= deadline) break;
        fd_set readable;
        FD_ZERO(&readable);
        for (SOCKET s : sockets) FD_SET(s, &readable);
        timeval wait{0, static_cast<long>(std::min<uint64_t>(deadline - now, 200'000))};
        if (select(0, &readable, nullptr, nullptr, &wait) <= 0) continue;

        for (SOCKET s : sockets) {
            if (!FD_ISSET(s, &readable)) continue;
            uint8_t data[512];
            sockaddr_in from{};
            int fromLength = sizeof from;
            int n = recvfrom(s, reinterpret_cast<char*>(data), sizeof data, 0, reinterpret_cast<sockaddr*>(&from), &fromLength);
            if (n < 7 + 2 + 32 || memcmp(data, kReply, 7) != 0) continue;
            uint8_t diff = 0;
            for (int i = 0; i < 32; ++i) diff |= data[9 + i] ^ expected[i];
            if (diff != 0) continue;  // 不持有同一 token 的设备，忽略

            wchar_t text[INET_ADDRSTRLEN];
            InetNtopW(AF_INET, &from.sin_addr, text, INET_ADDRSTRLEN);
            std::wstring address = text;
            bool duplicate = std::any_of(found.begin(), found.end(), [&](const DiscoveredHost& m) { return m.address == address; });
            if (duplicate) continue;
            DiscoveredHost mac;
            mac.address = address;
            mac.port = ReadU16(data + 7);
            mac.name = Utf8ToWide(std::string(reinterpret_cast<char*>(data + 41), static_cast<size_t>(n - 41)));
            mac.wireless = RouteIsWireless(reinterpret_cast<const sockaddr*>(&from));
            mac.replyMs = static_cast<int>((NowMicros() - start) / 1000);
            found.push_back(mac);
        }
    }
    for (SOCKET s : sockets) closesocket(s);

    std::stable_sort(found.begin(), found.end(), [](const DiscoveredHost& a, const DiscoveredHost& b) {
        if (a.wireless != b.wireless) return !a.wireless;
        return a.replyMs < b.replyMs;
    });
    return found;
}
