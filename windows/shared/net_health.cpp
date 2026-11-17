#include "net_health.h"
#include <iphlpapi.h>
#include <mstcpip.h>

namespace net {
namespace {
bool LinkLocal(const IN_ADDR& address) {
    return address.S_un.S_un_b.s_b1 == 169 && address.S_un.S_un_b.s_b2 == 254;
}
// 无法枚举时返回 true，交给 bind 判断。
template <typename Match>
bool AnyPreferred(Match match) {
    PMIB_UNICASTIPADDRESS_TABLE table = nullptr;
    if (GetUnicastIpAddressTable(AF_INET, &table) != NO_ERROR) return true;
    bool found = false;
    for (ULONG i = 0; i < table->NumEntries && !found; ++i) {
        const auto& row = table->Table[i];
        found = row.DadState == IpDadStatePreferred && match(row.Address.Ipv4.sin_addr);
    }
    FreeMibTable(table);
    return found;
}
}

bool AutoBind(const std::wstring& bind) { return _wcsicmp(bind.c_str(), L"auto") == 0; }
std::wstring ListenAddress(const std::wstring& bind) { return AutoBind(bind) ? L"0.0.0.0" : bind; }
bool PeerAllowed(const std::wstring& bind, const sockaddr_in& peer) {
    return !AutoBind(bind) || LinkLocal(peer.sin_addr);
}
bool HasLinkLocalAddress() {
    return AnyPreferred([](const IN_ADDR& address) { return LinkLocal(address); });
}

bool WiredLinkUp(uint64_t* mbps) {
    if (mbps) *mbps = 0;
    PMIB_IF_TABLE2 table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR) return true;
    bool up = false;
    for (ULONG i = 0; i < table->NumEntries && !up; ++i) {
        const auto& row = table->Table[i];
        up = row.Type == IF_TYPE_ETHERNET_CSMACD && row.InterfaceAndOperStatusFlags.HardwareInterface &&
             row.MediaConnectState == MediaConnectStateConnected;
        if (up && mbps) *mbps = row.ReceiveLinkSpeed / 1000000;
    }
    FreeMibTable(table);
    return up;
}

bool LocalAddressReady(const std::wstring& address) {
    IN_ADDR wanted{};
    if (InetPtonW(AF_INET, ListenAddress(address).c_str(), &wanted) != 1) return false;
    if (wanted.s_addr == INADDR_ANY || wanted.S_un.S_un_b.s_b1 == 127) return true;
    return AnyPreferred([&](const IN_ADDR& local) { return local.s_addr == wanted.s_addr; });
}

std::vector<std::wstring> LocalIPv4Addresses() {
    ULONG size = 16384;
    std::vector<uint8_t> buffer;
    ULONG status = ERROR_BUFFER_OVERFLOW;
    constexpr ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    for (int attempt = 0; attempt < 3 && status == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.resize(size);
        status = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                      reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (status != NO_ERROR) return {};

    std::vector<std::wstring> physical, other;
    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        MIB_IF_ROW2 row{};
        row.InterfaceLuid = adapter->Luid;
        const bool hardware = GetIfEntry2(&row) == NO_ERROR && row.InterfaceAndOperStatusFlags.HardwareInterface;
        auto& addresses = hardware ? physical : other;
        for (auto* entry = adapter->FirstUnicastAddress; entry; entry = entry->Next) {
            const auto* socketAddress = entry->Address.lpSockaddr;
            if (!socketAddress || socketAddress->sa_family != AF_INET || entry->DadState != IpDadStatePreferred) continue;
            const auto& ip = reinterpret_cast<const sockaddr_in*>(socketAddress)->sin_addr;
            if (ip.s_addr == INADDR_ANY || ip.S_un.S_un_b.s_b1 == 127) continue;
            wchar_t address[INET_ADDRSTRLEN]{};
            if (InetNtopW(AF_INET, &ip, address, INET_ADDRSTRLEN) &&
                std::find(addresses.begin(), addresses.end(), address) == addresses.end()) addresses.emplace_back(address);
        }
    }
    for (auto& address : other) {
        if (std::find(physical.begin(), physical.end(), address) == physical.end()) physical.push_back(std::move(address));
    }
    return physical;
}

bool AddressWatch::Lost() {
    const auto now = GetTickCount64();
    if (now < next_) return false;
    next_ = now + 2000;
    return !LocalAddressReady(address_);
}

std::string BindError(const std::wstring& address, uint16_t port, int code) {
    const auto text = WideToUtf8(address) + ":" + std::to_string(port);
    if (code == WSAEADDRNOTAVAIL || !LocalAddressReady(address)) return "本机网卡没有地址 " + text + "，等待网卡恢复";
    if (code == WSAEADDRINUSE || code == WSAEACCES) return "端口被占用 " + text;
    return "监听 " + text + " 失败：" + std::to_string(code);
}

void KeepAlive(SOCKET socket) {
    BOOL enabled = TRUE;
    setsockopt(socket, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&enabled), sizeof enabled);
    tcp_keepalive values{1, 10000, 2000};
    DWORD returned = 0;
    WSAIoctl(socket, SIO_KEEPALIVE_VALS, &values, sizeof values, nullptr, 0, &returned, nullptr, nullptr);
}
}
