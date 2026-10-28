#include "server.h"
#include "split_rules.h"

void ProxyServer::ServeVmPac(const proxy::Peer& local) {
    sockaddr_in address{};
    int size = sizeof address;
    char host[INET_ADDRSTRLEN]{};
    proxy::SystemSettings fallback;
    fallback.flags = 1;
    std::string script;
    if (getsockname(local->value, reinterpret_cast<sockaddr*>(&address), &size) ||
        !inet_ntop(AF_INET, &address.sin_addr, host, sizeof host) ||
        !proxy::BuildSplitPac(ini_, ntohs(address.sin_port), fallback, script, host)) {
        LogLine("生成虚拟机浏览器分流配置失败");
        proxy::Text(local->value, "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        return;
    }
    const auto header = "HTTP/1.1 200 OK\r\nContent-Type: application/x-ns-proxy-autoconfig\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: " + std::to_string(script.size()) + "\r\n\r\n";
    proxy::Text(local->value, header + script);
}
