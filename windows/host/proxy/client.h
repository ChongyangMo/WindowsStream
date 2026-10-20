#pragma once
#include "../../shared/proxy/transport.h"
#include "../config.h"

class HostProxyClient {
public:
    ~HostProxyClient() { Stop(); }
    void Start(const HostConfig& config);
    void Stop() { tasks_.Stop(); }
private:
    void Worker();
    std::wstring Host() const;
    proxy::Tasks tasks_;
    std::wstring host_;
    std::string token_;
    uint16_t port_ = 7403;
};
