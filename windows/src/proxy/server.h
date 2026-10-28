#pragma once
#include "request.h"
#include "vm_networks.h"
#include "tcp_forwards.h"
#include "../../shared/net_health.h"
#include <condition_variable>
#include <deque>

class ProxyServer {
public:
    ~ProxyServer() { Stop(); }
    void StartConfigured(const std::wstring& ini, const std::wstring& bind, const std::string& token);
    void Stop();
    bool Available();
    void SetVirtualMachinesAllowed(bool allowed) { vmAccess_.SetEnabled(allowed); }
    bool VirtualMachinesAllowed() const { return vmAccess_.Enabled(); }
private:
    void AcceptLoop(const proxy::Peer& listener, net::AddressWatch* watch);
    void TunnelLoop(const std::wstring& bind, uint16_t port);
    void Serve(const proxy::Peer& local, proxy::Tasks& requests, bool virtualMachine = false);
    void ServeVmPac(const proxy::Peer& local);
    void Forward(const proxy::Peer& local, proxy::Tasks& requests, const proxy::Request& request);
    proxy::Peer TakeTunnel(const std::atomic<bool>& requesting);
    proxy::Tasks tasks_;
    std::string token_;
    std::wstring tunnelBind_, ini_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<proxy::Peer> idle_;
    std::vector<std::weak_ptr<proxy::Socket>> authenticated_;
    proxy::VmAccess vmAccess_;
    proxy::TcpForwards tcpForwards_;
};
