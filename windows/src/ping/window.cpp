#include "window.h"
#include "../../shared/app_icon.h"

void IntranetPingWindow::Show(HWND owner, const std::wstring& ini) {
    if (window_) { ShowWindow(window_, SW_SHOWNORMAL); SetForegroundWindow(window_); return; }
    ini_ = ini;
    WNDCLASSW cls{};
    cls.lpfnWndProc = Procedure; cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"WindowsStreamIntranetPing";
    cls.hIcon = LoadAppIcon();
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&cls);
    window_ = CreateWindowExW(WS_EX_CONTROLPARENT, cls.lpszClassName, L"内网 Ping", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 600, 320, owner, nullptr, cls.hInstance, this);
    if (!window_) { LogLine("创建内网 Ping 窗口失败：%lu", GetLastError()); return; }
    target_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        16, 16, 440, 28, window_, nullptr, cls.hInstance, nullptr);
    button_ = CreateWindowW(L"BUTTON", L"Ping", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        468, 16, 96, 28, window_, reinterpret_cast<HMENU>(1), cls.hInstance, nullptr);
    output_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
        16, 60, 548, 204, window_, nullptr, cls.hInstance, nullptr);
    for (auto child : {target_, button_, output_}) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    SendMessageW(target_, EM_SETLIMITTEXT, 15, 0);
    wchar_t saved[32]{}; GetPrivateProfileStringW(L"intranet", L"ping_target", L"", saved, 32, ini_.c_str());
    SetWindowTextW(target_, saved);
    SetTimer(window_, 1, 100, nullptr);
    ShowWindow(window_, SW_SHOW); SetFocus(target_);
}
void IntranetPingWindow::Begin() {
    if (running_) return;
    wchar_t target[32]{}; GetWindowTextW(target_, target, 32);
    if (!intranet::ValidTarget(target)) { SetWindowTextW(output_, L"请输入 10.18 网段的 IPv4 地址"); return; }
    if (worker_.joinable()) worker_.join();
    tasks_.Stop(); tasks_.Start(); running_ = true;
    SetWindowTextW(output_, L""); EnableWindow(button_, FALSE);
    WritePrivateProfileStringW(L"intranet", L"ping_target", target, ini_.c_str());
    const auto port = intranet::ProxyPort(ini_);
    worker_ = std::thread([this, port, address = std::wstring(target)] {
        intranet::Ping(tasks_, port, address, [this](const std::wstring& line) {
            LogLine("内网 Ping：%s", WideToUtf8(line).c_str());
            std::lock_guard lock(mutex_); pending_.push_back(line);
        });
        running_ = false;
    });
}
void IntranetPingWindow::Poll() {
    std::vector<std::wstring> lines;
    { std::lock_guard lock(mutex_); lines.swap(pending_); }
    for (const auto& line : lines) {
        const auto text = line + L"\r\n";
        SendMessageW(output_, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
        SendMessageW(output_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
    }
    if (!running_) EnableWindow(button_, TRUE);
}
void IntranetPingWindow::Close() {
    tasks_.Stop();
    if (worker_.joinable()) worker_.join();
    if (window_) { auto window = window_; window_ = nullptr; DestroyWindow(window); }
    { std::lock_guard lock(mutex_); pending_.clear(); }
}
LRESULT CALLBACK IntranetPingWindow::Procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto self = reinterpret_cast<IntranetPingWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<IntranetPingWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self) {
        if (message == WM_COMMAND && LOWORD(wParam) == 1 && HIWORD(wParam) == BN_CLICKED) { self->Begin(); return 0; }
        if (message == WM_TIMER) { self->Poll(); return 0; }
        if (message == WM_CLOSE) { self->Close(); return 0; }
        if (message == WM_NCDESTROY) self->window_ = nullptr;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
