#include "tcp_forwards.h"
#include "../../shared/net_health.h"

namespace proxy {
namespace {
std::wstring Value(const std::wstring& ini, const wchar_t* section, const wchar_t* key) {
    wchar_t value[512]{};
    GetPrivateProfileStringW(section, key, L"", value, 512, ini.c_str());
    return value;
}
uint16_t Port(const std::wstring& value) {
    if (value.empty() || value.size() > 5 || value.find_first_not_of(L"0123456789") != std::wstring::npos) return 0;
    const auto number = wcstoul(value.c_str(), nullptr, 10);
    return number && number <= 65535 ? static_cast<uint16_t>(number) : 0;
}
bool ValidHost(const std::string& host) {
    return !host.empty() && host.size() <= 253 &&
        host.find_first_of("@/\\?#[]") == std::string::npos &&
        std::none_of(host.begin(), host.end(), [](unsigned char c) { return c <= 32 || c >= 127; });
}
}
void TcpForwards::Start(const std::wstring& ini, Forward forward) {
    Stop();
    std::vector<wchar_t> sections(32768);
    if (GetPrivateProfileSectionNamesW(sections.data(), static_cast<DWORD>(sections.size()), ini.c_str()) >= sections.size() - 2) {
        LogLine("TCP 转发配置过长，未启动固定端口转发"); return;
    }
    forward_ = std::move(forward);
    tasks_.Start();
    std::vector<uint16_t> ports;
    for (const auto* name = sections.data(); *name; name += wcslen(name) + 1) {
        if (_wcsnicmp(name, L"tcp-forward:", 12) || !name[12] ||
            !GetPrivateProfileIntW(name, L"enabled", 0, ini.c_str())) continue;
        const auto localPort = Port(Value(ini, name, L"listen_port"));
        const auto targetPort = Port(Value(ini, name, L"target_port"));
        const auto host = WideToUtf8(Value(ini, name, L"target_host"));
        if (!localPort || !targetPort || !ValidHost(host) ||
            std::find(ports.begin(), ports.end(), localPort) != ports.end()) {
            LogLine("TCP 转发配置无效或本机端口重复：%s", WideToUtf8(name).c_str()); continue;
        }
        if (ports.size() >= 16) { LogLine("TCP 转发最多启用十六个入口，其余配置未启动"); break; }
        ports.push_back(localPort);
        tasks_.Run([this, section = std::wstring(name), localPort, host, targetPort] {
            ListenLoop(section, localPort, host, targetPort);
        });
    }
}
void TcpForwards::ListenLoop(std::wstring section, uint16_t localPort, std::string host, uint16_t targetPort) {
    int previousError = 0;
    while (tasks_.running) {
        int code = 0;
        auto listener = Listen(tasks_, L"127.0.0.1", localPort, &code);
        if (!listener) {
            if (code != previousError) LogLine("TCP 转发暂不可用：%s，%s，稍后自动重试",
                WideToUtf8(section).c_str(), net::BindError(L"127.0.0.1", localPort, code).c_str());
            previousError = code;
            for (int i = 0; i < 10 && tasks_.running; ++i) Sleep(200);
            continue;
        }
        previousError = 0;
        LogLine("TCP 转发已监听：127.0.0.1:%u，经被控端连接 %s:%u", localPort, host.c_str(), targetPort);
        while (tasks_.running) {
            if (!Ready(listener->value, false, 200)) continue;
            auto peer = tasks_.Track(accept(listener->value, nullptr, nullptr));
            if (!peer) continue;
            Timeout(peer->value, 15000);
            if (!tasks_.Run([this, peer, host, targetPort] { forward_(peer, tasks_, host, targetPort); }))
                LogLine("TCP 转发请求过多，已关闭新连接：%s:%u", host.c_str(), targetPort);
        }
    }
}
}
