#include "vm_networks.h"
#include <iphlpapi.h>
#include <cwctype>

namespace proxy {
namespace {
bool VmwareAdapter(const IP_ADAPTER_ADDRESSES& adapter) {
    std::wstring text = adapter.Description ? adapter.Description : L"";
    text += L" ";
    text += adapter.FriendlyName ? adapter.FriendlyName : L"";
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return text.find(L"vmware") != std::wstring::npos &&
        (text.find(L"vmnet") != std::wstring::npos || text.find(L"virtual ethernet") != std::wstring::npos);
}
bool PrivateAddress(uint32_t ip) {
    return (ip & 0xff000000u) == 0x0a000000u || (ip & 0xfff00000u) == 0xac100000u ||
        (ip & 0xffff0000u) == 0xc0a80000u;
}
}
bool VmNetwork::Allows(const sockaddr_in& peer) const {
    const auto ip = ntohl(peer.sin_addr.s_addr);
    return peer.sin_family == AF_INET && (ip & mask) == network && ip != network && ip != (network | ~mask);
}
bool ReadVmNetworks(std::vector<VmNetwork>& networks) {
    networks.clear();
    ULONG size = 16384;
    std::vector<unsigned char> buffer(size);
    ULONG result = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.resize(size);
        result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
            GAA_FLAG_SKIP_DNS_SERVER, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (result == ERROR_NO_DATA) return true;
    if (result != NO_ERROR) return false;
    for (auto adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || !VmwareAdapter(*adapter)) continue;
        for (auto entry = adapter->FirstUnicastAddress; entry; entry = entry->Next) {
            if (!entry->Address.lpSockaddr || entry->Address.lpSockaddr->sa_family != AF_INET ||
                entry->DadState != IpDadStatePreferred || entry->OnLinkPrefixLength < 8 || entry->OnLinkPrefixLength > 30) continue;
            const auto ip = ntohl(reinterpret_cast<sockaddr_in*>(entry->Address.lpSockaddr)->sin_addr.s_addr);
            if (!PrivateAddress(ip)) continue;
            wchar_t address[INET_ADDRSTRLEN]{};
            InetNtopW(AF_INET, &reinterpret_cast<sockaddr_in*>(entry->Address.lpSockaddr)->sin_addr, address, INET_ADDRSTRLEN);
            const auto mask = 0xffffffffu << (32 - entry->OnLinkPrefixLength);
            const VmNetwork network{address, ip & mask, mask};
            if (std::none_of(networks.begin(), networks.end(), [&](const auto& item) { return item.address == network.address; }))
                networks.push_back(network);
        }
    }
    return true;
}
}
