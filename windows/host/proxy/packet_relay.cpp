#include "packet_relay.h"
#include <iphlpapi.h>
#include <icmpapi.h>

namespace proxy {
uint32_t PacketCapabilities() {
    uint32_t flags = 1;
    SOCKET udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp != INVALID_SOCKET) { flags |= 2; closesocket(udp); }
    HANDLE icmp = IcmpCreateFile();
    if (icmp != INVALID_HANDLE_VALUE) { flags |= 4; IcmpCloseHandle(icmp); }
    LogLine("IP 隧道实际接口检测：TCP 可用，UDP %s，真实回显 %s", flags & 2 ? "可用" : "不可用", flags & 4 ? "可用" : "不可用");
    return 0x00010000 | flags;
}
bool IntranetIPv4(const std::string& host) {
    IN_ADDR address{};
    return inet_pton(AF_INET, host.c_str(), &address) == 1 &&
        (ntohl(address.s_addr) & 0xffff0000u) == 0x0a120000u;
}
void RelayDatagrams(Tasks& tasks, SOCKET tunnel, const std::string& host, uint16_t port) {
    auto target = tasks.Track(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &address.sin_addr);
    uint8_t result = target && connect(target->value, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0 ? 0 : 1;
    if (!Send(tunnel, &result, 1) || result) return;
    Timeout(target->value, 5000);
    std::vector<uint8_t> bytes(65507);
    auto last = GetTickCount64();
    while (tasks.running && GetTickCount64() - last < 60000) {
        fd_set readable; FD_ZERO(&readable); FD_SET(tunnel, &readable); FD_SET(target->value, &readable);
        timeval wait{0, 200000};
        if (select(0, &readable, nullptr, nullptr, &wait) == SOCKET_ERROR) return;
        if (FD_ISSET(tunnel, &readable)) {
            uint8_t header[2]{};
            if (!Read(tunnel, header, 2)) return;
            const auto size = ReadU16(header);
            if (size > bytes.size() || !Read(tunnel, bytes.data(), size) ||
                send(target->value, reinterpret_cast<const char*>(bytes.data()), size, 0) != size) return;
            last = GetTickCount64();
        }
        if (FD_ISSET(target->value, &readable)) {
            int size = recv(target->value, reinterpret_cast<char*>(bytes.data()), static_cast<int>(bytes.size()), 0);
            if (size < 0) return;
            Writer frame; frame.u16(static_cast<uint16_t>(size)); frame.append(bytes.data(), size);
            if (!Send(tunnel, frame.bytes.data(), frame.bytes.size())) return;
            last = GetTickCount64();
        }
    }
}
void RelayEcho(SOCKET tunnel, const std::string& host) {
    HANDLE icmp = IcmpCreateFile();
    uint8_t opened = icmp != INVALID_HANDLE_VALUE ? 0 : 1;
    if (!Send(tunnel, &opened, 1) || opened) { if (icmp != INVALID_HANDLE_VALUE) IcmpCloseHandle(icmp); return; }
    uint8_t header[4]{};
    if (!Read(tunnel, header, 4)) { IcmpCloseHandle(icmp); return; }
    const auto size = ReadU16(header + 2);
    if (size > 65000 || !header[0]) { IcmpCloseHandle(icmp); return; }
    std::vector<uint8_t> data(size), reply(sizeof(ICMP_ECHO_REPLY) + size + 8);
    if (!Read(tunnel, data.data(), data.size())) { IcmpCloseHandle(icmp); return; }
    IN_ADDR target{}; inet_pton(AF_INET, host.c_str(), &target);
    IP_OPTION_INFORMATION options{}; options.Ttl = header[0]; options.Flags = header[1] & IP_FLAG_DF;
    DWORD count = IcmpSendEcho(icmp, target.s_addr, data.data(), size, &options, reply.data(), static_cast<DWORD>(reply.size()), 2000);
    const DWORD error = count ? 0 : GetLastError();
    const auto* echo = reinterpret_cast<const ICMP_ECHO_REPLY*>(reply.data());
    Writer response;
    response.u32(count ? echo->Status : error);
    if (count) response.append(&echo->Address, 4); else response.u32(0);
    response.u32(count ? echo->RoundTripTime : 0);
    response.u8(count ? echo->Options.Ttl : 0);
    const bool success = count && echo->Status == IP_SUCCESS && echo->DataSize <= size;
    response.u16(success ? echo->DataSize : 0);
    if (success) response.append(echo->Data, echo->DataSize);
    if (!success) LogLine("内网真实回显未成功：%s，状态 %lu", host.c_str(), count ? echo->Status : error);
    Send(tunnel, response.bytes.data(), response.bytes.size());
    IcmpCloseHandle(icmp);
}
}
