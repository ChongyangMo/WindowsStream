#include "net_health.h"
#include <iphlpapi.h>
#include <mstcpip.h>
#include <utility>

namespace net {
namespace {
bool LinkLocal(const IN_ADDR& address) {
    return address.S_un.S_un_b.s_b1 == 169 && address.S_un.S_un_b.s_b2 == 254;
}

std::wstring IPv4Text(const IN_ADDR& address) {
    if (address.s_addr == INADDR_ANY) return {};
    wchar_t text[INET_ADDRSTRLEN]{};
    return InetNtopW(AF_INET, &address, text, INET_ADDRSTRLEN) ? text : L"";
}

std::wstring RouteSource(const SOCKADDR_INET& destination, ULONG interfaceIndex) {
    MIB_IPFORWARD_ROW2 route{};
    SOCKADDR_INET source{};
    if (GetBestRoute2(nullptr, interfaceIndex, nullptr, &destination, 0, &route, &source) != NO_ERROR ||
        source.si_family != AF_INET) return {};
    return IPv4Text(source.Ipv4.sin_addr);
}

struct LanAddress {
    std::wstring first, gatewaySource;
    bool multiple = false;
    ULONG gatewayMetric = 0;
};

std::vector<uint8_t> ReadIPv4Adapters(ULONG extraFlags) {
    ULONG size = 16384;
    std::vector<uint8_t> buffer;
    ULONG status = ERROR_BUFFER_OVERFLOW;
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | extraFlags;
    for (int attempt = 0; attempt < 3 && status == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.resize(size);
        status = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                      reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (status != NO_ERROR) return {};
    return buffer;
}

bool UsableAdapter(const IP_ADAPTER_ADDRESSES& adapter) {
    return adapter.OperStatus == IfOperStatusUp && adapter.IfType != IF_TYPE_SOFTWARE_LOOPBACK;
}

bool HardwareInterface(const IP_ADAPTER_ADDRESSES& adapter) {
    MIB_IF_ROW2 row{};
    row.InterfaceLuid = adapter.Luid;
    return GetIfEntry2(&row) == NO_ERROR && row.InterfaceAndOperStatusFlags.HardwareInterface;
}

const IN_ADDR* PreferredUnicastIPv4(const IP_ADAPTER_UNICAST_ADDRESS& entry) {
    const auto* socketAddress = entry.Address.lpSockaddr;
    if (!socketAddress || socketAddress->sa_family != AF_INET || entry.DadState != IpDadStatePreferred) return nullptr;
    return &reinterpret_cast<const sockaddr_in*>(socketAddress)->sin_addr;
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

std::wstring SourceIPv4Address(const std::wstring& peer) {
    SOCKADDR_INET destination{};
    destination.Ipv4.sin_family = AF_INET;
    if (InetPtonW(AF_INET, peer.c_str(), &destination.Ipv4.sin_addr) != 1 ||
        destination.Ipv4.sin_addr.s_addr == INADDR_ANY) return {};
    ULONG interfaceIndex = 0;
    if (GetBestInterfaceEx(reinterpret_cast<sockaddr*>(&destination.Ipv4), &interfaceIndex) != NO_ERROR) return {};
    return RouteSource(destination, interfaceIndex);
}

std::wstring PreferredIntranetIPv4Address() {
    const auto buffer = ReadIPv4Adapters(0);
    if (buffer.empty()) return {};

    std::wstring preferred;
    std::pair<bool, ULONG> bestRank{};
    for (auto* adapter = reinterpret_cast<const IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter; adapter = adapter->Next) {
        if (!UsableAdapter(*adapter)) continue;
        for (auto* entry = adapter->FirstUnicastAddress; entry; entry = entry->Next) {
            const auto* ip = PreferredUnicastIPv4(*entry);
            if (!ip || (ntohl(ip->s_addr) & 0xffff0000u) != 0x0a120000u) continue;
            const auto rank = std::pair{!HardwareInterface(*adapter), adapter->Ipv4Metric};
            if (preferred.empty() || rank < bestRank) {
                preferred = IPv4Text(*ip);
                bestRank = rank;
            }
            break;
        }
    }
    return preferred;
}

std::wstring PreferredLanIPv4Address() {
    const auto buffer = ReadIPv4Adapters(GAA_FLAG_INCLUDE_GATEWAYS);
    if (buffer.empty()) return {};

    LanAddress physical, other;
    for (auto* adapter = reinterpret_cast<const IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter; adapter = adapter->Next) {
        if (!UsableAdapter(*adapter)) continue;
        auto& candidate = HardwareInterface(*adapter) ? physical : other;
        for (auto* entry = adapter->FirstUnicastAddress; entry; entry = entry->Next) {
            const auto* ip = PreferredUnicastIPv4(*entry);
            if (!ip || ip->S_un.S_un_b.s_b1 == 127) continue;
            const auto address = IPv4Text(*ip);
            if (address.empty()) continue;
            if (candidate.first.empty()) candidate.first = address;
            else if (candidate.first != address) candidate.multiple = true;
        }
        if (!candidate.gatewaySource.empty() && adapter->Ipv4Metric >= candidate.gatewayMetric) continue;
        for (auto* gateway = adapter->FirstGatewayAddress; gateway; gateway = gateway->Next) {
            const auto* address = gateway->Address.lpSockaddr;
            if (!address || address->sa_family != AF_INET) continue;
            SOCKADDR_INET destination{};
            destination.Ipv4 = *reinterpret_cast<const sockaddr_in*>(address);
            auto source = RouteSource(destination, adapter->IfIndex);
            if (source.empty()) continue;
            candidate.gatewaySource = std::move(source);
            candidate.gatewayMetric = adapter->Ipv4Metric;
            break;
        }
    }
    const auto& preferred = physical.first.empty() && physical.gatewaySource.empty() ? other : physical;
    if (!preferred.gatewaySource.empty()) return preferred.gatewaySource;
    return preferred.multiple ? L"" : preferred.first;
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
