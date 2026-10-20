#include "locator.h"
#include "../../shared/locate.h"
#include <iphlpapi.h>

namespace {
void Probe(SOCKET socket, ULONG address, const std::vector<uint8_t>& request) {
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(locate::kPort);
    target.sin_addr.s_addr = address;
    sendto(socket, reinterpret_cast<const char*>(request.data()), static_cast<int>(request.size()), 0,
           reinterpret_cast<const sockaddr*>(&target), sizeof target);
}

}

// 只在 169.254 链路本地网卡上广播，不向公司网络等其他网段发送；已知地址另外单播询问。
std::wstring FindController(const std::string& token, const std::vector<std::wstring>& known, const std::atomic<bool>& running) {
    uint8_t nonce[16]{};
    const auto request = locate::Request(token, nonce);
    if (request.empty()) return {};
    ULONG size = 32 * 1024;
    std::vector<uint8_t> buffer(size);
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG status = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    if (status == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        status = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    std::vector<SOCKET> sockets;
    for (auto* adapter = status == NO_ERROR ? reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()) : nullptr; adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
            const auto* local = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
            if (local->sin_family != AF_INET || local->sin_addr.S_un.S_un_b.s_b1 != 169 || local->sin_addr.S_un.S_un_b.s_b2 != 254) continue;
            SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (socket == INVALID_SOCKET) continue;
            BOOL on = TRUE;
            setsockopt(socket, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof on);
            sockaddr_in bound = *local;
            bound.sin_port = 0;
            if (bind(socket, reinterpret_cast<const sockaddr*>(&bound), sizeof bound) != 0) { closesocket(socket); continue; }
            Probe(socket, htonl(0xA9FEFFFFu), request);  // 169.254.255.255
            Probe(socket, INADDR_BROADCAST, request);
            sockets.push_back(socket);
        }
    }
    if (SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); socket != INVALID_SOCKET) {
        for (const auto& host : known) {
            IN_ADDR address{};
            if (InetPtonW(AF_INET, host.c_str(), &address) == 1) Probe(socket, address.s_addr, request);
        }
        sockets.push_back(socket);
    }
    std::wstring found;
    const auto deadline = GetTickCount64() + 1500;
    while (found.empty() && running && GetTickCount64() < deadline) {
        fd_set readable;
        FD_ZERO(&readable);
        for (SOCKET socket : sockets) FD_SET(socket, &readable);
        timeval wait{0, 200000};
        if (select(0, &readable, nullptr, nullptr, &wait) <= 0) continue;
        for (SOCKET socket : sockets) {
            if (!FD_ISSET(socket, &readable)) continue;
            uint8_t answer[128]{};
            sockaddr_in from{};
            int fromLength = sizeof from;
            const int received = recvfrom(socket, reinterpret_cast<char*>(answer), sizeof answer, 0,
                                          reinterpret_cast<sockaddr*>(&from), &fromLength);
            uint16_t port = 0;
            if (received <= 0 || !locate::CheckAnswer(token, nonce, answer, static_cast<size_t>(received), port)) continue;
            wchar_t text[INET_ADDRSTRLEN]{};
            InetNtopW(AF_INET, &from.sin_addr, text, INET_ADDRSTRLEN);
            found = text;
            break;
        }
    }
    for (SOCKET socket : sockets) closesocket(socket);
    return found;
}

ControllerLocator& Locator() {
    static ControllerLocator locator;
    return locator;
}

void ControllerLocator::Init(const HostConfig& config) {
    std::lock_guard lock(mutex_);
    ini_ = PortableDir() + L"\\host.ini";
    token_ = config.token;
    configured_ = config.controllerHost;
    wchar_t buffer[64]{};
    GetPrivateProfileStringW(L"connection", L"discovered_host", L"", buffer, 64, ini_.c_str());
    IN_ADDR check{};
    if (InetPtonW(AF_INET, buffer, &check) == 1) saved_ = buffer;
    current_ = configured_;
}

std::wstring ControllerLocator::Current() {
    std::lock_guard lock(mutex_);
    return current_;
}

void ControllerLocator::Succeeded() {
    std::lock_guard lock(mutex_);
    nextFind_ = 0;
    misses_ = 0;
    if (current_ == saved_ || current_.empty()) return;
    saved_ = current_;
    if (WritePrivateProfileStringW(L"connection", L"discovered_host", saved_.c_str(), ini_.c_str()))
        LogLine("已记录控制端地址 %s", WideToUtf8(saved_).c_str());
}

void ControllerLocator::Find(const std::atomic<bool>& running) {
    std::vector<std::wstring> known;
    {
        std::lock_guard lock(mutex_);
        if (token_.empty() || GetTickCount64() < nextFind_) return;
        known = {configured_};
        if (!saved_.empty() && saved_ != configured_) known.push_back(saved_);
    }
    const auto found = FindController(token_, known, running);
    std::lock_guard lock(mutex_);
    // 连续找不到时逐步拉长间隔，最多 30 秒一次。
    misses_ = found.empty() ? std::min(misses_ + 1, 6u) : 0;
    nextFind_ = GetTickCount64() + 5000ull * (misses_ ? misses_ : 1);
    std::wstring next = found;
    if (next.empty() && !saved_.empty() && saved_ != configured_) next = current_ == saved_ ? configured_ : saved_;
    if (next.empty() || next == current_) return;
    LogLine(found.empty() ? "未收到控制端应答，改试地址 %s" : "自动发现控制端：%s", WideToUtf8(next).c_str());
    current_ = next;
}
