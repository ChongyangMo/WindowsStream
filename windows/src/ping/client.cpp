#include "client.h"
#include "../../shared/crypto.h"
#include <bcrypt.h>

namespace intranet {
uint16_t ProxyPort(const std::wstring& ini) {
    const auto port = GetPrivateProfileIntW(L"proxy", L"local_port", 1080, ini.c_str());
    return port > 0 && port <= 65535 ? static_cast<uint16_t>(port) : 0;
}
bool ValidTarget(const std::wstring& target) {
    IN_ADDR address{};
    return InetPtonW(AF_INET, target.c_str(), &address) == 1 &&
        (ntohl(address.s_addr) & 0xffff0000u) == 0x0a120000u;
}
EchoResult Echo(proxy::Tasks& tasks, uint16_t port, const std::wstring& target) {
    if (!port || !ValidTarget(target)) return {false, L"请输入 10.18 网段的 IPv4 地址"};
    uint8_t connected = 1;
    auto peer = proxy::Connect(tasks, L"127.0.0.1", port, connected);
    if (!peer) return {false, L"无法连接本机代理，请先启动 A 控制端"};
    proxy::Timeout(peer->value, 12000);
    const auto host = WideToUtf8(target);
    Writer request;
    request.u8(127); request.u8(3); request.u16(0); request.u16(static_cast<uint16_t>(host.size()));
    request.append(host.data(), host.size());
    uint8_t status = 1;
    if (!proxy::Send(peer->value, request.bytes.data(), request.bytes.size()) || !proxy::Read(peer->value, &status, 1))
        return {false, L"B 的回显通道不可用，请确认 B 已更新并连接"};
    if (status) return {false, L"B 的回显通道未就绪，状态 " + std::to_wstring(status)};
    // 每个请求携带新的随机数据，只接受 B 返回的真实目标回包。
    uint8_t payload[32]{};
    if (BCryptGenRandom(nullptr, payload, sizeof payload, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        return {false, L"生成回显校验数据失败"};
    Writer echo;
    echo.u8(128); echo.u8(0); echo.u16(sizeof payload); echo.append(payload, sizeof payload);
    uint8_t header[15]{};
    if (!proxy::Send(peer->value, echo.bytes.data(), echo.bytes.size()) || !proxy::Read(peer->value, header, sizeof header))
        return {false, L"等待 B 回传结果失败，通道已断开或超时"};
    const auto result = ReadU32(header);
    if (result) {
        const wchar_t* reason = result == 11010 ? L"请求超时" : result == 11002 ? L"网络不可达" :
            result == 11003 ? L"主机不可达" : result == 11013 ? L"TTL 已过期" : L"回显失败";
        return {false, std::wstring(reason) + L"，B 返回状态 " + std::to_wstring(result)};
    }
    uint8_t actual[sizeof payload]{};
    if (ReadU16(header + 13) != sizeof payload || !proxy::Read(peer->value, actual, sizeof actual) ||
        memcmp(payload, actual, sizeof payload) != 0) return {false, L"回显数据校验失败"};
    IN_ADDR address{}; memcpy(&address, header + 4, sizeof address);
    wchar_t source[INET_ADDRSTRLEN]{};
    if (!InetNtopW(AF_INET, &address, source, INET_ADDRSTRLEN) || target != source)
        return {false, L"回显来源与目标地址不一致"};
    return {true, std::wstring(L"来自 ") + source + L"：32 字节，B 往返 " + std::to_wstring(ReadU32(header + 8)) +
        L" ms，TTL=" + (header[12] ? std::to_wstring(header[12]) : L"未知")};
}
void Ping(proxy::Tasks& tasks, uint16_t port, const std::wstring& target,
          const std::function<void(const std::wstring&)>& output) {
    if (!ValidTarget(target) || !port) { output(L"请输入 10.18 网段的 IPv4 地址，并检查代理端口"); return; }
    unsigned sent = 0, received = 0;
    output(L"经 B Ping " + target);
    for (unsigned i = 0; i < 4 && tasks.running; ++i) {
        const auto reply = Echo(tasks, port, target);
        if (!tasks.running) break;
        ++sent; received += reply.success ? 1 : 0;
        output(reply.message);
        for (unsigned wait = 0; i < 3 && wait < 10 && tasks.running; ++wait) Sleep(100);
    }
    output(L"已完成 " + std::to_wstring(sent) + L" 次，收到 " + std::to_wstring(received) + L" 次，失败 " +
        std::to_wstring(sent - received) + L" 次");
}
}
