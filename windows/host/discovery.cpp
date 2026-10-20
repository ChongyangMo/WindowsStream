#include "discovery.h"
#include "../shared/crypto.h"
#include <iphlpapi.h>

DiscoveryResponder::~DiscoveryResponder() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void DiscoveryResponder::Start(const HostConfig& config) {
    if (config.reverse) return;  // 主动连接模式没有对外监听的被控端端口。
    running_ = true;
    thread_ = std::thread([this, config] {
        SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket == INVALID_SOCKET) return;
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = htons(7401);
        local.sin_addr.s_addr = INADDR_ANY;
        if (bind(socket, reinterpret_cast<sockaddr*>(&local), sizeof local) != 0) {
            LogLine("局域网发现监听失败：%d", WSAGetLastError());
            closesocket(socket);
            return;
        }
        ULONG length = 16384;
        std::vector<uint8_t> buffer(length);
        auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        ULONG result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr, adapters, &length);
        if (result == ERROR_BUFFER_OVERFLOW) {
            buffer.resize(length);
            adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
            result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr, adapters, &length);
        }
        if (result == NO_ERROR) {
            for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
                if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
                for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
                    if (unicast->Address.lpSockaddr->sa_family != AF_INET) continue;
                    ip_mreq membership{};
                    InetPtonW(AF_INET, L"239.255.77.87", &membership.imr_multiaddr);
                    membership.imr_interface = reinterpret_cast<sockaddr_in*>(unicast->Address.lpSockaddr)->sin_addr;
                    setsockopt(socket, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&membership), sizeof membership);
                }
            }
        }
        wchar_t name[MAX_COMPUTERNAME_LENGTH + 1]{};
        DWORD nameLength = MAX_COMPUTERNAME_LENGTH + 1;
        GetComputerNameW(name, &nameLength);
        const auto computer = WideToUtf8(name);
        while (running_) {
            fd_set readable;
            FD_ZERO(&readable); FD_SET(socket, &readable);
            timeval wait{0, 250000};
            if (select(0, &readable, nullptr, nullptr, &wait) <= 0) continue;
            sockaddr_in from{};
            int fromLength = sizeof from;
            uint8_t request[512]{};
            const int received = recvfrom(socket, reinterpret_cast<char*>(request), sizeof request, 0,
                                           reinterpret_cast<sockaddr*>(&from), &fromLength);
            if (received != 23 || memcmp(request, "WSDISC2", 7) != 0) continue;
            const std::string context = "windowsstream-discover";
            Writer proof;
            proof.append(context.data(), context.size());
            proof.append(request + 7, 16);
            uint8_t digest[32]{};
            if (!HmacSha256(config.token, proof.bytes.data(), proof.bytes.size(), digest)) continue;
            Writer response;
            response.append("WSHERE2", 7);
            response.u16(config.port);
            response.append(digest, sizeof digest);
            response.append(computer.data(), computer.size());
            sendto(socket, reinterpret_cast<const char*>(response.bytes.data()), static_cast<int>(response.bytes.size()), 0,
                   reinterpret_cast<const sockaddr*>(&from), fromLength);
        }
        closesocket(socket);
    });
}
