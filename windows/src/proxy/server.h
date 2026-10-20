#pragma once
#include "request.h"
#include "../../shared/net_health.h"
#include <condition_variable>
#include <deque>

class ProxyServer {
public:
    ~ProxyServer() { Stop(); }
    void StartConfigured(const std::wstring& ini, const std::wstring& bind, const std::string& token);
    void Stop();
    bool Available();
private:
    void AcceptLoop(const proxy::Peer& listener, net::AddressWatch* watch);
    void TunnelLoop(const std::wstring& bind, uint16_t port);
    void Serve(const proxy::Peer& local);
    proxy::Peer TakeTunnel();
    proxy::Tasks tasks_;
    std::string token_;
    std::wstring tunnelBind_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<proxy::Peer> idle_;
    std::vector<std::weak_ptr<proxy::Socket>> authenticated_;
};
