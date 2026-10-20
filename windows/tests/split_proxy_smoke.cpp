#include "../src/proxy/pac_server.h"
#include "../src/proxy/split_rules.h"
#include <winhttp.h>
#include <cstdio>

int wmain(int count, wchar_t** arguments) {
    proxy::SystemSettings original;
    if (count == 2) {
        proxy::ProxySnapshot snapshot;
        if (!proxy::LoadSnapshot(arguments[1], snapshot)) return 1;
        original = snapshot.original;
    } else if (!proxy::ReadSystemSettings(original)) return 1;
    std::string script;
    if (!proxy::BuildSplitPac(PortableDir() + L"\\client.ini", 1080, original, script)) return 2;
    proxy::PacServer pac;
    if (!pac.Start(script)) return 3;
    auto session = WinHttpOpen(L"WindowsStream分流验证", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
    if (!session) return 4;
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 10000);
    auto resolve = [&](const wchar_t* target, bool intranet) {
        WINHTTP_AUTOPROXY_OPTIONS options{};
        options.dwFlags = WINHTTP_AUTOPROXY_CONFIG_URL;
        options.lpszAutoConfigUrl = pac.Url().c_str();
        WINHTTP_PROXY_INFO info{};
        bool ok = WinHttpGetProxyForUrl(session, target, &options, &info) != FALSE;
        if (!ok) printf("PAC_ERROR=%lu\n", GetLastError());
        const bool viaB = ok && info.dwAccessType == WINHTTP_ACCESS_TYPE_NAMED_PROXY && info.lpszProxy &&
            std::wstring(info.lpszProxy) == L"127.0.0.1:1080";
        const bool direct = ok && info.dwAccessType == WINHTTP_ACCESS_TYPE_NO_PROXY;
        wprintf(L"%s -> %s\n", target, viaB ? L"B" : direct ? L"A" : L"解析失败");
        if (info.lpszProxy) GlobalFree(info.lpszProxy);
        if (info.lpszProxyBypass) GlobalFree(info.lpszProxyBypass);
        return intranet ? viaB : original.flags == 1 ? direct : ok && !viaB;
    };
    bool ok = resolve(L"https://10.18.0.0/", true) && resolve(L"https://10.18.255.255/", true) &&
        resolve(L"https://10.17.255.255/", false) && resolve(L"https://10.19.0.0/", false) &&
        resolve(L"https://192.168.50.2/", false) && resolve(L"https://www.google.com/", false) &&
        resolve(L"https://sdserver.luxvisions-inno.com/", true);
    auto connection = WinHttpConnect(session, L"sdserver.luxvisions-inno.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    auto request = connection ? WinHttpOpenRequest(connection, L"GET", L"/", nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE) : nullptr;
    WINHTTP_AUTOPROXY_OPTIONS options{}; options.dwFlags = WINHTTP_AUTOPROXY_CONFIG_URL; options.lpszAutoConfigUrl = pac.Url().c_str();
    WINHTTP_PROXY_INFO info{};
    DWORD status = 0, bytes = sizeof status;
    const bool reachable = request && WinHttpGetProxyForUrl(session, L"https://sdserver.luxvisions-inno.com/", &options, &info) &&
        WinHttpSetOption(request, WINHTTP_OPTION_PROXY, &info, sizeof info) &&
        WinHttpSendRequest(request, nullptr, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr) &&
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &bytes, nullptr) && status == 200;
    printf("SD_HTTP=%lu\n", status);
    if (info.lpszProxy) GlobalFree(info.lpszProxy);
    if (info.lpszProxyBypass) GlobalFree(info.lpszProxyBypass);
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session); pac.Stop();
    return ok && reachable ? 0 : 5;
}
