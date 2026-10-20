#include "session.h"
#include "update/client.h"
#include "update/apply.h"
#include "runtime/runtime.h"
#include "proxy/client.h"
#include "locate/locator.h"
#include "../shared/host_control.h"
#include "../shared/app_icon.h"
#include <shellapi.h>
#include <thread>

namespace {
constexpr UINT kStatus = WM_APP + 1;
HostServer server;
HostRuntime runtime;
HWND statusLabel = nullptr;

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    LRESULT runtimeResult = 0;
    if (runtime.Handle(message, wParam, lParam, runtimeResult)) return runtimeResult;
    switch (message) {
        case WM_CREATE:
            statusLabel = CreateWindowW(L"STATIC", L"正在启动", WS_CHILD | WS_VISIBLE | SS_CENTER,
                                        20, 24, 240, 30, window, nullptr, nullptr, nullptr);
            CreateWindowW(L"BUTTON", L"隐藏", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                          40, 68, 90, 30, window, reinterpret_cast<HMENU>(1), nullptr, nullptr);
            CreateWindowW(L"BUTTON", L"退出", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                          150, 68, 90, 30, window, reinterpret_cast<HMENU>(2), nullptr, nullptr);
            return 0;
        case kStatus: {
            std::unique_ptr<std::wstring> status(reinterpret_cast<std::wstring*>(lParam));
            SetWindowTextW(statusLabel, status->c_str());
            runtime.Status(*status);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    int argumentCount = 0;
    auto arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments && argumentCount == 6 && std::wstring(arguments[1]) == L"--watchdog") {
        const DWORD parent = wcstoul(arguments[2], nullptr, 10);
        const auto created = _wcstoui64(arguments[3], nullptr, 10);
        const auto parentHandle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_wcstoui64(arguments[4], nullptr, 10)));
        const auto stopHandle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_wcstoui64(arguments[5], nullptr, 10)));
        LocalFree(arguments);
        return RunRecovery(parent, created, parentHandle, stopHandle);
    }
    if (arguments && argumentCount == 3 && std::wstring(arguments[1]) == L"--apply-update") {
        const auto ticket = std::wstring(arguments[2]); LocalFree(arguments);
        return updates::Apply(ticket);
    }
    std::string bootId;
    const bool background = arguments && argumentCount == 2 && std::wstring(arguments[1]) == L"--background";
    if (arguments && argumentCount == 3 && std::wstring(arguments[1]) == L"--updated") bootId = WideToUtf8(arguments[2]);
    if (arguments) LocalFree(arguments);
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\WindowsStreamHost");
    if (!single || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (single) CloseHandle(single);
        if (!updates::BootstrapExisting() && !background) {
            if (HWND existing = FindWindowW(L"WindowsStreamHostWindow", nullptr)) {
                ShowWindow(existing, SW_RESTORE); PostMessageW(existing, kHostShowWindow, 0, 0); SetForegroundWindow(existing);
            }
        }
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HostConfig config;
    std::wstring error;
    if (!LoadHostConfig(config, error)) {
        MessageBoxW(nullptr, error.c_str(), L"WindowsStream", MB_OK | MB_ICONERROR);
        CloseHandle(single);
        return 1;
    }
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { CloseHandle(single); return 1; }
    WNDCLASSW type{};
    type.hInstance = instance;
    type.lpszClassName = L"WindowsStreamHostWindow";
    type.lpfnWndProc = WindowProc;
    type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.hIcon = LoadAppIcon();
    RegisterClassW(&type);
    RECT area{0, 0, 280, 120};
    AdjustWindowRect(&area, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    HWND window = CreateWindowW(type.lpszClassName, L"WindowsStream 被控端",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, area.right - area.left, area.bottom - area.top,
                                nullptr, nullptr, instance, nullptr);
    if (!window) { WSACleanup(); CloseHandle(single); return 1; }
    runtime.Start(window, config, background, show, [window] { server.Stop(); DestroyWindow(window); });
    LogLine("Windows 被控端启动，模式：%s", config.reverse ? "主动连接控制端" : "等待控制端连接");
    Locator().Init(config);
    HostUpdateClient updateClient;
    updateClient.Start(config, bootId);
    HostProxyClient proxyClient;
    proxyClient.Start(config);
    std::thread worker([&] {
        server.Run(config, window, [window](const std::wstring& status) {
            auto* copy = new std::wstring(status);
            if (!PostMessageW(window, kStatus, 0, reinterpret_cast<LPARAM>(copy))) delete copy;
        });
    });
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    server.Stop();
    runtime.Stop();
    updateClient.Stop();
    proxyClient.Stop();
    worker.join();
    WSACleanup();
    CloseHandle(single);
    return 0;
}
