#pragma once
#include "../../shared/proxy/transport.h"

namespace proxy {
struct VmNetwork {
    std::wstring address;
    uint32_t network = 0, mask = 0;
    bool Allows(const sockaddr_in& peer) const;
};
// 仅发现宿主机的 VMware 私有虚拟网卡，不开放物理网卡。
bool ReadVmNetworks(std::vector<VmNetwork>& networks);

class VmAccess {
public:
    using Handler = std::function<void(const Peer&, Tasks&)>;
    ~VmAccess() { Stop(); }
    void Start(uint16_t port, bool enabled, Handler handler);
    void SetEnabled(bool enabled) { enabled_ = enabled; }
    bool Enabled() const { return enabled_; }
    void Stop();
private:
    void Loop(uint16_t port, const Handler& handler);
    std::atomic<bool> running_{false}, enabled_{true};
    std::thread worker_;
    Tasks requests_;
};
}
