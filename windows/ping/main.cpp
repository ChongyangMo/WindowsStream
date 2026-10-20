#include "../src/ping/client.h"
#include <cstdio>

namespace {
void Print(const std::wstring& text) {
    const auto line = text + L"\r\n";
    const auto handle = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0, written = 0;
    if (GetConsoleMode(handle, &mode)) WriteConsoleW(handle, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    else { const auto utf8 = WideToUtf8(line); WriteFile(handle, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr); }
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2 || !intranet::ValidTarget(argv[1])) {
        Print(L"用法：WindowsStreamPing.exe <10.18 网段 IPv4 地址>"); return 2;
    }
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) { Print(L"初始化网络失败"); return 2; }
    proxy::Tasks tasks; tasks.Start();
    bool success = false;
    intranet::Ping(tasks, intranet::ProxyPort(PortableDir() + L"\\client.ini"), argv[1], [&](const auto& line) {
        Print(line); if (line.starts_with(L"来自 ")) success = true;
    });
    tasks.Stop(); WSACleanup();
    return success ? 0 : 1;
}
