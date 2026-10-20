#pragma once
#include "server.h"
#include "system_proxy.h"
#include "../tunnel/manager.h"
#include "../ping/window.h"

class NetworkAccess {
public:
    void Start(HWND window, const std::wstring& ini, ProxyServer& server);
    void Tick(bool connected);
    bool Handle(UINT message, WPARAM wParam);
    void Stop();
private:
    void Update();
    HWND window_ = nullptr;
    HMENU menu_ = nullptr;
    std::wstring ini_, label_;
    ProxyServer* server_ = nullptr;
    proxy::SystemProxy lease_;
    IpTunnel tunnel_;
    IntranetPingWindow ping_;
    bool useTunnel_ = false;
    uint16_t port_ = 1080;
    bool enabled_ = true, connected_ = false, error_ = false, suspending_ = false;
    uint64_t retry_ = 0;
};
