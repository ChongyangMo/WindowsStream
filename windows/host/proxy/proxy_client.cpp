#include "client.h"
#include "packet_relay.h"
#include "../locate/locator.h"

void HostProxyClient::Start(const HostConfig& config) {
    const auto ini = PortableDir() + L"\\host.ini";
    if (!GetPrivateProfileIntW(L"proxy", L"enabled", 1, ini.c_str())) return;
    // 未单独指定时跟随自动发现得到的控制端地址。
    wchar_t host[512]{};
    GetPrivateProfileStringW(L"proxy", L"controller_host", L"", host, 512, ini.c_str());
    const UINT port = GetPrivateProfileIntW(L"proxy", L"tunnel_port", 7403, ini.c_str());
    const UINT connections = GetPrivateProfileIntW(L"proxy", L"connections", 64, ini.c_str());
    if ((!*host && config.controllerHost.empty()) || !port || port > 65535 || !connections || connections > 64 || config.token.size() < 32) {
        LogLine("内网代理配置无效，请检查控制端地址、端口和连接数量"); return;
    }
    host_ = host; token_ = config.token; port_ = static_cast<uint16_t>(port);
    tasks_.Start();
    for (UINT i = 0; i < connections; ++i) tasks_.Run([this] { Worker(); });
    LogLine("内网代理已启动，主动连接控制端 %s:%u，连接数量 %u", WideToUtf8(Host()).c_str(), port_, connections);
}
std::wstring HostProxyClient::Host() const { return host_.empty() ? Locator().Current() : host_; }
void HostProxyClient::Worker() {
    bool failed = false;
    while (tasks_.running) {
        uint8_t result = 1;
        auto tunnel = proxy::Connect(tasks_, Host(), port_, result);
        if (!tunnel || !proxy::Authenticate(tunnel->value, token_, false)) {
            if (!failed && tasks_.running) LogLine("内网代理连接或认证失败，稍后重试");
            failed = true;
            for (int i = 0; i < 25 && tasks_.running; ++i) Sleep(200);
            continue;
        }
        if (failed) LogLine("内网代理连接已恢复");
        failed = false;
        // 空闲时不占用视频连接；退出通过任务管理器 shutdown 解除读取。
        uint8_t command = 0;
        while (tasks_.running && !proxy::Ready(tunnel->value, false, 200)) {}
        if (!tasks_.running || !proxy::Read(tunnel->value, &command, 1)) continue;
        if (command == 4) {
            Writer capabilities; capabilities.u8(0); capabilities.u32(proxy::PacketCapabilities());
            proxy::Send(tunnel->value, capabilities.bytes.data(), capabilities.bytes.size()); continue;
        }
        if (command < 1 || command > 3) continue;
        uint8_t header[4]{};
        if (!proxy::Read(tunnel->value, header, sizeof header)) continue;
        const uint16_t port = ReadU16(header), length = ReadU16(header + 2);
        if ((!port && command != 3) || !length || length > 253) continue;
        std::string host(length, '\0');
        if (!proxy::Read(tunnel->value, host.data(), length) ||
            std::any_of(host.begin(), host.end(), [](unsigned char c) { return c <= 32 || c >= 127; })) continue;
        if (command != 1) {
            if (!proxy::IntranetIPv4(host)) { const uint8_t denied = 2; proxy::Send(tunnel->value, &denied, 1); continue; }
            if (command == 2) proxy::RelayDatagrams(tasks_, tunnel->value, host, port);
            else proxy::RelayEcho(tunnel->value, host);
            continue;
        }
        auto target = proxy::Connect(tasks_, Utf8ToWide(host), port, result);
        if (!target) LogLine("内网代理目标连接失败：%s:%u，状态 %u", host.c_str(), port, result);
        if (!proxy::Send(tunnel->value, &result, 1) || !target) continue;
        proxy::Relay(tasks_, tunnel->value, target->value);
    }
}
