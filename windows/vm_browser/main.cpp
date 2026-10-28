#include "../src/proxy/system_settings.h"
#include <cstdio>
#include <wininet.h>

namespace {
bool Endpoint(std::wstring& url) {
    const auto ini = PortableDir() + L"\\client.ini";
    wchar_t host[32]{};
    GetPrivateProfileStringW(L"proxy", L"host", L"", host, 32, ini.c_str());
    const auto port = GetPrivateProfileIntW(L"proxy", L"port", 0, ini.c_str());
    IN_ADDR address{};
    if (InetPtonW(AF_INET, host, &address) != 1 || !port || port > 65535) return false;
    const auto ip = ntohl(address.s_addr);
    if ((ip & 0xff000000u) != 0x0a000000u && (ip & 0xfff00000u) != 0xac100000u &&
        (ip & 0xffff0000u) != 0xc0a80000u) return false;
    url = L"http://" + std::wstring(host) + L":" + std::to_wstring(port) + L"/windowsstream.pac";
    return true;
}
bool CheckPac(const std::wstring& url) {
    auto session = InternetOpenW(L"WindowsStream虚拟机配置", INTERNET_OPEN_TYPE_DIRECT, nullptr, nullptr, 0);
    if (!session) return false;
    DWORD timeout = 8000;
    InternetSetOptionW(session, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof timeout);
    InternetSetOptionW(session, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof timeout);
    auto request = InternetOpenUrlW(session, url.c_str(), nullptr, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_AUTO_REDIRECT, 0);
    DWORD status = 0, size = sizeof status;
    char prefix[16]{}; DWORD received = 0;
    const bool ok = request && HttpQueryInfoW(request, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
        &status, &size, nullptr) && status == 200 && InternetReadFile(request, prefix, sizeof prefix, &received) &&
        std::string(prefix, received).starts_with("function ");
    if (request) InternetCloseHandle(request);
    InternetCloseHandle(session);
    return ok;
}
}

int wmain(int count, wchar_t** arguments) {
    SetConsoleOutputCP(CP_UTF8);
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) return 11;
    struct Cleanup { ~Cleanup() { WSACleanup(); } } cleanup;
    const auto directory = PortableDir();
    if (directory.empty()) { puts("未找到同目录的 client.ini 配置。"); return 1; }
    const auto backup = directory + L"\\.windowsstream-vm-browser.ini";
    if (count == 2 && std::wstring(arguments[1]) == L"--restore") {
        const bool ok = proxy::RestoreSnapshot(backup);
        puts(ok ? "已恢复虚拟机原来的浏览器代理设置。" : "恢复失败，原设置备份已保留。");
        return ok ? 0 : 2;
    }
    std::wstring url;
    if (!Endpoint(url)) { puts("虚拟机代理地址配置无效。"); return 3; }
    if (!CheckPac(url)) { puts("无法读取宿主机分流配置，请检查 WindowsStream 的虚拟机访问开关。"); return 4; }
    proxy::SystemSettings current;
    if (!proxy::ReadSystemSettings(current)) return 5;
    proxy::ProxySnapshot previous;
    if (GetFileAttributesW(backup.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (!proxy::LoadSnapshot(backup, previous)) { puts("原设置备份读取失败，未修改浏览器配置。"); return 6; }
        if (current == previous.applied && current.script == url) { puts("虚拟机浏览器自动分流已配置。"); return 0; }
        if (!proxy::RestoreSnapshot(backup) || !proxy::ReadSystemSettings(current)) return 7;
    }
    if ((current.flags & 2) || ((current.flags & 4) && !current.script.empty())) {
        puts("虚拟机已有其他代理配置，未覆盖现有设置。"); return 8;
    }
    proxy::ProxySnapshot state;
    state.process = GetCurrentProcessId(); state.created = proxy::Created(GetCurrentProcess());
    state.original = current; state.applied = current;
    state.applied.flags = 1 | 4; state.applied.script = url;
    if (!state.created || !proxy::SaveSnapshot(backup, state)) return 9;
    if (!proxy::WriteSystemSettings(state.applied)) {
        proxy::WriteSystemSettings(state.original);
        proxy::RestoreSnapshot(backup);
        puts("设置浏览器自动分流失败，已尝试恢复原设置。"); return 10;
    }
    puts("虚拟机浏览器自动分流已启用，内网经宿主机访问，其他地址直连。");
    LogLine("虚拟机浏览器自动分流已启用：%s", WideToUtf8(url).c_str());
    return 0;
}
