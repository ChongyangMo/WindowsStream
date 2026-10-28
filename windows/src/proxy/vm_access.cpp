#include "vm_networks.h"
#include "../../shared/net_health.h"
#include <map>

namespace proxy {
void VmAccess::Start(uint16_t port, bool enabled, Handler handler) {
    Stop();
    enabled_ = enabled; running_ = true;
    worker_ = std::thread([this, port, handler = std::move(handler)] {
        try { Loop(port, handler); }
        catch (...) { LogLine("虚拟机代理异常停止，本机代理保持运行"); }
        requests_.Stop();
    });
}
void VmAccess::Stop() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
}
void VmAccess::Loop(uint16_t port, const Handler& handler) {
    struct Listener { VmNetwork network; Peer peer; };
    std::vector<Listener> listeners;
    std::map<std::wstring, int> errors;
    ULONGLONG refresh = 0;
    bool missingReported = false, enumerationFailed = false;
    while (running_) {
        if (!enabled_) {
            if (requests_.running) requests_.Stop();
            listeners.clear(); errors.clear(); refresh = 0;
            Sleep(100); continue;
        }
        if (!requests_.running) requests_.Start();
        const auto now = GetTickCount64();
        if (now >= refresh) {
            refresh = now + 2000;
            std::vector<VmNetwork> networks;
            if (!ReadVmNetworks(networks)) {
                if (!enumerationFailed) LogLine("枚举虚拟网卡失败，暂停虚拟机代理并稍后重试");
                enumerationFailed = true;
                requests_.Stop(); listeners.clear();
                Sleep(100); continue;
            }
            enumerationFailed = false;
            if (networks.empty() && !missingReported) LogLine("未发现可用的 VMware 私有虚拟网卡，等待网卡就绪");
            missingReported = networks.empty();
            std::erase_if(listeners, [&](const auto& item) {
                const bool removed = std::none_of(networks.begin(), networks.end(), [&](const auto& network) {
                    return item.network.address == network.address && item.network.mask == network.mask && item.network.network == network.network;
                });
                if (removed) LogLine("虚拟机代理监听已移除：%s:%u", WideToUtf8(item.network.address).c_str(), port);
                return removed;
            });
            for (const auto& network : networks) {
                if (std::any_of(listeners.begin(), listeners.end(), [&](const auto& item) { return item.network.address == network.address; })) continue;
                int code = 0;
                auto listener = Listen(requests_, network.address, port, &code);
                if (!listener) {
                    const auto found = errors.find(network.address);
                    if (found == errors.end() || found->second != code)
                        LogLine("虚拟机代理暂不可用：%s，稍后重试", net::BindError(network.address, port, code).c_str());
                    errors[network.address] = code; continue;
                }
                u_long nonblocking = 1;
                if (ioctlsocket(listener->value, FIONBIO, &nonblocking)) {
                    LogLine("设置虚拟机代理监听失败：%d", WSAGetLastError()); continue;
                }
                errors.erase(network.address);
                listeners.push_back({network, listener});
                LogLine("虚拟机代理已监听：%s:%u，仅接受该虚拟网卡同网段的连接", WideToUtf8(network.address).c_str(), port);
            }
        }
        for (const auto& listener : listeners) {
            if (!running_ || !enabled_) break;
            if (!Ready(listener.peer->value, false, 0)) continue;
            sockaddr_in from{}; int size = sizeof from;
            auto peer = requests_.Track(accept(listener.peer->value, reinterpret_cast<sockaddr*>(&from), &size));
            if (!peer || !listener.network.Allows(from)) continue;
            // accept 继承监听器的非阻塞状态，现有请求解析使用阻塞读和超时。
            u_long nonblocking = 0;
            if (ioctlsocket(peer->value, FIONBIO, &nonblocking)) continue;
            Timeout(peer->value, 5000);
            requests_.Run([this, peer, handler] { if (enabled_) handler(peer, requests_); });
        }
        Sleep(100);
    }
}
}
