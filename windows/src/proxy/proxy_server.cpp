#include "server.h"

void ProxyServer::StartConfigured(const std::wstring& ini, const std::wstring& bind, const std::string& token) {
    if (!GetPrivateProfileIntW(L"proxy", L"enabled", 1, ini.c_str()) || token.size() < 32) return;
    const UINT localPort = GetPrivateProfileIntW(L"proxy", L"local_port", 1080, ini.c_str());
    const UINT tunnelPort = GetPrivateProfileIntW(L"proxy", L"tunnel_port", 7403, ini.c_str());
    if (!localPort || localPort > 65535 || !tunnelPort || tunnelPort > 65535 || localPort == tunnelPort) {
        LogLine("内网代理端口配置无效"); return;
    }
    token_ = token; tunnelBind_ = bind; tasks_.Start();
    int code = 0;
    auto local = proxy::Listen(tasks_, L"127.0.0.1", static_cast<uint16_t>(localPort), &code);
    if (!local) {
        LogLine("内网代理未能启动：%s", net::BindError(L"127.0.0.1", static_cast<uint16_t>(localPort), code).c_str());
        Stop(); return;
    }
    tasks_.Run([this, local] { AcceptLoop(local, nullptr); });
    tasks_.Run([this, bind, tunnelPort] { TunnelLoop(bind, static_cast<uint16_t>(tunnelPort)); });
    LogLine("内网代理已启动：本机 127.0.0.1:%u，等待被控端反向连接端口 %u", localPort, tunnelPort);
}
// 被控端通道监听绑定在网线地址上；地址缺失或被移除时持续重试，不需要重启控制端。
void ProxyServer::TunnelLoop(const std::wstring& bind, uint16_t port) {
    std::string lastError;
    while (tasks_.running) {
        int code = 0;
        auto listener = net::LocalAddressReady(bind) ? proxy::Listen(tasks_, net::ListenAddress(bind), port, &code) : proxy::Peer{};
        if (!listener) {
            const auto error = net::BindError(bind, port, code ? code : WSAEADDRNOTAVAIL);
            if (error != lastError) LogLine("内网代理被控端通道暂不可用：%s，稍后自动重试", error.c_str());
            lastError = error;
            for (int i = 0; i < 10 && tasks_.running; ++i) Sleep(200);
            continue;
        }
        if (!lastError.empty()) LogLine("内网代理被控端通道恢复监听：%s:%u", WideToUtf8(bind).c_str(), port);
        lastError.clear();
        net::AddressWatch watch(bind);
        AcceptLoop(listener, &watch);
        listener.reset();
        if (!tasks_.running) break;
        LogLine("内网代理监听地址已移除，等待网卡恢复");
        lastError = "地址已移除";
        // 旧地址上的空闲通道已不可用，主动关闭，避免把请求分配给失效连接。
        std::lock_guard lock(mutex_);
        for (const auto& peer : idle_) shutdown(peer->value, SD_BOTH);
        idle_.clear();
    }
}
void ProxyServer::Stop() {
    tasks_.running = false; ready_.notify_all(); tasks_.Stop();
    std::lock_guard lock(mutex_); idle_.clear(); authenticated_.clear();
}
bool ProxyServer::Available() {
    std::lock_guard lock(mutex_);
    std::erase_if(idle_, [](const auto& peer) { return proxy::Ready(peer->value, false, 0); });
    std::erase_if(authenticated_, [](const auto& peer) { return peer.expired(); });
    return tasks_.running && !authenticated_.empty();
}
void ProxyServer::AcceptLoop(const proxy::Peer& listener, net::AddressWatch* watch) {
    const bool tunnel = watch != nullptr;
    while (tasks_.running) {
        if (watch && watch->Lost()) return;
        if (!proxy::Ready(listener->value, false, 200)) continue;
        sockaddr_in from{};
        int fromLength = sizeof from;
        auto peer = tasks_.Track(accept(listener->value, reinterpret_cast<sockaddr*>(&from), &fromLength));
        if (!peer) continue;
        if (tunnel && !net::PeerAllowed(tunnelBind_, from)) continue;  // 离开作用域即关闭
        proxy::Timeout(peer->value, 5000);
        tasks_.Run([this, peer, tunnel] {
            if (!tunnel) { Serve(peer); return; }
            if (!proxy::Authenticate(peer->value, token_, true)) { LogLine("内网代理被控端认证失败"); return; }
            std::lock_guard lock(mutex_);
            std::erase_if(idle_, [](const auto& item) { return proxy::Ready(item->value, false, 0); });
            if (idle_.size() >= 64 || !tasks_.running) return;
            const bool first = idle_.empty();
            idle_.push_back(peer); ready_.notify_one();
            authenticated_.push_back(peer);
            if (first) LogLine("内网代理被控端通道已就绪");
        });
    }
}
proxy::Peer ProxyServer::TakeTunnel() {
    std::unique_lock lock(mutex_);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (tasks_.running) {
        while (!idle_.empty()) {
            auto peer = idle_.front(); idle_.pop_front();
            // 空闲通道不应发送数据；可读表示断开或协议错误，丢弃后等待重连。
            if (!proxy::Ready(peer->value, false, 0)) return peer;
        }
        if (ready_.wait_until(lock, deadline) == std::cv_status::timeout) break;
    }
    return {};
}
void ProxyServer::Serve(const proxy::Peer& local) {
    proxy::Request request;
    if (!proxy::Parse(local->value, request)) return;
    auto tunnel = TakeTunnel();
    if (!tunnel) { LogLine("内网代理请求失败：被控端通道不可用"); proxy::Reply(local->value, request, 3); return; }
    proxy::Timeout(tunnel->value, 15000);
    Writer command; command.u8(request.command);
    if (request.command != 4) {
        command.u16(request.port); command.u16(static_cast<uint16_t>(request.host.size()));
        command.append(request.host.data(), request.host.size());
    }
    uint8_t result = 1;
    if (!proxy::Send(tunnel->value, command.bytes.data(), command.bytes.size()) || !proxy::Read(tunnel->value, &result, 1)) result = 1;
    if (!proxy::Reply(local->value, request, result) || result) {
        LogLine("内网代理连接失败：%s:%u，状态 %u", request.host.c_str(), request.port, result); return;
    }
    if (!request.initial.empty() && !proxy::Text(tunnel->value, request.initial)) return;
    if (request.command == 4) LogLine("IP 隧道能力检测已转发");
    else LogLine("内网转发通道：%s:%u，类型 %u", request.host.c_str(), request.port, request.command);
    proxy::Relay(tasks_, local->value, tunnel->value);
}
