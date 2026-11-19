#include "session.h"
#include "version.h"
#include "exclusive_mouse.h"
#include "update/client.h"
#include "update/apply.h"
#include "runtime/autorun.h"
#include "runtime/runtime.h"
#include "ui/host_view.h"
#include "proxy/client.h"
#include "service/service.h"
#include "locate/locator.h"
#include "../shared/host_control.h"
#include "../shared/app_icon.h"
#include "../shared/net_health.h"
#include <shellapi.h>
#include <string_view>
#include <thread>

namespace {
constexpr UINT kStatus = WM_APP + 1;
constexpr UINT_PTR kAddressTimer = 33;
constexpr DWORD kWindowStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
HostServer server;
HostRuntime runtime;
HostView view;
std::wstring displayedAddress;

std::wstring LocalAddress(const HostConfig& config) {
    auto address = net::PreferredIntranetIPv4Address();
    if (!address.empty()) return address;
    address = server.ConnectedLocalAddress();
    if (!address.empty()) return address;
    if (config.reverse) return net::SourceIPv4Address(Locator().Current());
    if (config.bindHost != L"0.0.0.0") return net::LocalAddressReady(config.bindHost) ? config.bindHost : L"";
    address = net::SourceIPv4Address(config.controllerHost);
    return address.empty() ? net::PreferredLanIPv4Address() : address;
}

void RefreshLocalAddress(HWND window) {
    const auto* config = reinterpret_cast<const HostConfig*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!config) return;
    auto text = LocalAddress(*config);
    if (text.empty()) text = L"未确定连接地址";
    if (text == displayedAddress) return;
    view.SetAddress(text);
    displayedAddress = std::move(text);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    LRESULT runtimeResult = 0;
    if (runtime.Handle(message, wParam, lParam, runtimeResult)) return runtimeResult;
    switch (message) {
        case WM_CREATE: {
            const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
            if (!view.Create(window, *static_cast<const HostConfig*>(creation->lpCreateParams))) return -1;
            RefreshLocalAddress(window);
            SetTimer(window, kAddressTimer, 3000, nullptr);
            return 0;
        }
        case WM_DPICHANGED:
            view.ResizeForDpi(LOWORD(wParam), reinterpret_cast<const RECT*>(lParam));
            return 0;
        case WM_PAINT:
            view.Paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_CTLCOLORSTATIC:
            return reinterpret_cast<LRESULT>(view.ControlColor(reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam)));
        case WM_SYSCOLORCHANGE:
        case WM_SETTINGCHANGE:
            view.SetColors();
            break;
        case WM_SHOWWINDOW:
            if (wParam) RefreshLocalAddress(window);
            break;
        case WM_TIMER:
            if (wParam != kAddressTimer) break;
            if (IsWindowVisible(window)) RefreshLocalAddress(window);
            return 0;
        case kStatus: {
            std::unique_ptr<std::wstring> status(reinterpret_cast<std::wstring*>(lParam));
            view.SetStatus(*status);
            runtime.Status(*status);
            if (IsWindowVisible(window)) RefreshLocalAddress(window);
            return 0;
        }
        case kHostExclusiveMouseStatus: {
            const wchar_t* status = wParam ? L"鼠标独占中 · Ctrl+Alt+F11 解除" : L"已连接";
            view.SetStatus(status);
            runtime.Status(status);
            LogLine("%s", wParam ? "已启用远程独占鼠标" : "已恢复现场鼠标");
            return 0;
        }
        case WM_DESTROY:
            KillTimer(window, kAddressTimer);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    const int serverResult = servermode::Dispatch();
    if (serverResult >= 0) return serverResult;
    int argumentCount = 0;
    auto arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments && argumentCount >= 2 && std::wstring(arguments[1]) == L"--watchdog") {
        if (argumentCount != 8) { LocalFree(arguments); return 2; }
        const DWORD parent = wcstoul(arguments[2], nullptr, 10);
        const auto created = _wcstoui64(arguments[3], nullptr, 10);
        const auto parentHandle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_wcstoui64(arguments[4], nullptr, 10)));
        const auto stopHandle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_wcstoui64(arguments[5], nullptr, 10)));
        const auto uiBeat = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_wcstoui64(arguments[6], nullptr, 10)));
        const auto workerBeat = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_wcstoui64(arguments[7], nullptr, 10)));
        LocalFree(arguments);
        return RunRecovery(parent, created, parentHandle, stopHandle, uiBeat, workerBeat);
    }
    if (arguments && argumentCount == 3 && std::wstring(arguments[1]) == L"--apply-update") {
        const auto ticket = std::wstring(arguments[2]); LocalFree(arguments);
        return updates::Apply(ticket);
    }
    if (arguments && argumentCount == 2 && std::wstring_view(arguments[1]) == kRelayAutoStartArgument) {
        LocalFree(arguments);
        return LaunchRelayAutoStart();
    }
    std::string bootId;
    const bool background = arguments && argumentCount == 2 && std::wstring(arguments[1]) == L"--background";
    const bool updated = arguments && argumentCount == 3 && std::wstring(arguments[1]) == L"--updated";
    if (updated) bootId = WideToUtf8(arguments[2]);
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
    Locator().Init(config);
    WNDCLASSW type{};
    type.hInstance = instance;
    type.lpszClassName = L"WindowsStreamHostWindow";
    type.lpfnWndProc = WindowProc;
    type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.hIcon = LoadAppIcon();
    RegisterClassW(&type);
    const auto area = HostView::WindowBounds(kWindowStyle, GetDpiForSystem());
    HWND window = CreateWindowW(type.lpszClassName, L"WindowsStream 被控端", kWindowStyle,
                                CW_USEDEFAULT, CW_USEDEFAULT, area.right - area.left, area.bottom - area.top,
                                nullptr, nullptr, instance, &config);
    if (!window) { WSACleanup(); CloseHandle(single); return 1; }
    view.ResizeForDpi(GetDpiForWindow(window));
    runtime.Start(window, config, background || (updated && config.startHidden), show,
                  [window] { server.Stop(); DestroyWindow(window); }, [] { return server.Progress(); });
    LogLine("WindowsStream 被控端版本：%s，模式：%s", WideToUtf8(HostVersion()).c_str(),
        config.reverse ? "主动连接控制端" : "等待控制端连接");
    LogLine("文字剪贴板：%s，双向文件拖拽：%s",
        config.clipboard ? "已启用" : "已关闭", config.fileDrag ? "已启用" : "已关闭");
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
