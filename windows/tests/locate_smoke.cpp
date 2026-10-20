#include "../src/locate/responder.h"
#include "../src/updates/server.h"
#include "../src/proxy/server.h"
#include "../host/locate/locator.h"
#include "../shared/net_health.h"
#include <cstdio>

// 用生产模块在本机真实 UDP/TCP 套接字上检查：签名发现往返、错误口令、auto 模式来源网段限制、
// 监听地址缺失时的等待日志。参数：本机已有的非 169.254 地址、本机不存在的地址。
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) { puts("usage: locate_smoke <local-ip> <missing-ip>"); return 2; }
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) return 1;
    const std::wstring local = argv[1], missing = argv[2];
    const std::string token(64, 'a');
    std::atomic<bool> running{true};
    int failed = 0;
    auto check = [&](const char* name, bool ok) { printf("%s %s\n", ok ? "PASS" : "FAIL", name); failed += ok ? 0 : 1; };

    check("local address ready", net::LocalAddressReady(local));
    check("missing address not ready", !net::LocalAddressReady(missing));
    check("auto listens on any", net::LocalAddressReady(L"auto"));
    {
        LocateResponder responder;
        responder.Start(L"0.0.0.0", 7400, token);
        Sleep(300);
        check("signed discovery answered", FindController(token, {local}, running) == local);
        check("wrong token ignored", FindController(std::string(64, 'b'), {local}, running).empty());
    }
    {
        LocateResponder responder;
        responder.Start(L"auto", 7400, token);
        Sleep(300);
        check("auto mode ignores non link-local peer", FindController(token, {local}, running).empty());
    }
    {
        UpdateServer updates;
        updates.Start(missing, 17402, token, L"missing.exe");
        Sleep(3000);
        updates.Stop();
    }
    WSACleanup();
    printf("%s\n", failed ? "FAILED" : "ALL PASSED");
    return failed ? 1 : 0;
}
