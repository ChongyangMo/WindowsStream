#include "../src/ping/window.h"
#include <cstdio>

// 使用生产窗口与真实 B 通道，仅执行 ICMP，不启动桌面、升级或发布操作。
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) return 2;
    IntranetPingWindow panel;
    panel.Show(nullptr, argv[1]);
    const auto window = FindWindowW(L"WindowsStreamIntranetPing", nullptr);
    const auto target = FindWindowExW(window, nullptr, L"EDIT", nullptr);
    const auto output = FindWindowExW(window, target, L"EDIT", nullptr);
    if (!window || !target || !output) return 3;
    SetWindowTextW(target, L"10.18.32.67");
    SendMessageW(window, WM_COMMAND, 1, 0);
    const auto deadline = GetTickCount64() + 15000;
    std::wstring result;
    while (GetTickCount64() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        wchar_t text[4096]{};
        GetWindowTextW(output, text, 4096);
        result = text;
        if (result.find(L"已完成 4 次") != std::wstring::npos) break;
        Sleep(10);
    }
    const bool passed = result.find(L"收到 4 次") != std::wstring::npos;
    const auto text = WideToUtf8(result);
    fwrite(text.data(), 1, text.size(), stdout);
    panel.Close();
    WSACleanup();
    return passed ? 0 : 1;
}
