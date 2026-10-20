#include "../src/proxy/system_proxy.h"
#include <cstdio>
#include <wininet.h>

int wmain(int count, wchar_t** arguments) {
    int guard = proxy::RunProxyGuard();
    if (guard >= 0) return guard;
    if (count == 2 && std::wstring(arguments[1]) == L"--recover")
        return proxy::RestoreSnapshot(PortableDir() + L"\\.windowsstream-proxy.ini") ? 0 : 7;
    proxy::SystemSettings original;
    if (!proxy::ReadSystemSettings(original)) return 1;
    proxy::SystemProxy lease;
    if (!lease.Start(1080) || !lease.Owns()) return 2;
    puts("ACTIVE"); fflush(stdout);
    if (count == 2 && std::wstring(arguments[1]) == L"--hold") { getchar(); return lease.Stop() ? 0 : 3; }
    // 不提供代理参数，使用当前用户系统代理访问真实内网页面。
    auto session = InternetOpenW(L"WindowsStream验证", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    DWORD timeout = 15000;
    if (session) {
        InternetSetOptionW(session, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof timeout);
        InternetSetOptionW(session, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof timeout);
    }
    auto request = session ? InternetOpenUrlW(session, L"https://sdserver.luxvisions-inno.com/", nullptr, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_AUTO_REDIRECT, 0) : nullptr;
    DWORD status = 0, size = sizeof status;
    bool ok = request && HttpQueryInfoW(request, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &size, nullptr) && status == 200;
    if (request) InternetCloseHandle(request);
    if (session) InternetCloseHandle(session);
    printf("STATUS=%lu\n", status);
    if (!lease.Stop()) return 4;
    proxy::SystemSettings restored;
    if (!proxy::ReadSystemSettings(restored) || !(restored == original)) return 5;
    puts("RESTORED"); return ok ? 0 : 6;
}
