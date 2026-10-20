#include "../src/proxy/server.h"
#include "../host/proxy/client.h"
#include <cstdio>

// 只启动生产代理模块，不启动桌面采集、守护或用户设置修改。
int main(int argc, char** argv) {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) return 1;
    const auto directory = PortableDir();
    HostConfig config;
    std::wstring error;
    if (!LoadHostConfig(config, error)) return 2;
    ProxyServer server;
    HostProxyClient host;
    const bool hostOnly = argc == 2 && std::string(argv[1]) == "--host-only";
    const bool serverOnly = argc == 2 && std::string(argv[1]) == "--server-only";
    if (!hostOnly) server.StartConfigured(directory + L"\\client.ini", L"127.0.0.1", config.token);
    if (!serverOnly) host.Start(config);
    puts("READY"); fflush(stdout);
    getchar();
    host.Stop(); server.Stop();
    WSACleanup();
    return 0;
}
