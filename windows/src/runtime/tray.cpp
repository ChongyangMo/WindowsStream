#include "tray.h"
#include "../../shared/app_icon.h"

void ControllerTray::Start(HWND window) {
    window_ = window;
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    Add();
}
bool ControllerTray::Add() {
    if (added_) return true;
    retry_ = GetTickCount64() + 10000;
    icon_ = {};
    icon_.cbSize = sizeof icon_; icon_.hWnd = window_; icon_.uID = 1;
    icon_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    icon_.uCallbackMessage = kControllerTrayCallback;
    icon_.hIcon = LoadAppIcon(true);
    wcscpy_s(icon_.szTip, L"WindowsStream 控制端");
    added_ = Shell_NotifyIconW(NIM_ADD, &icon_) != FALSE;
    if (added_) {
        icon_.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &icon_);
        LogLine("控制端托盘图标已启用");
    } else LogLine("添加控制端托盘图标失败：%lu，保留可见窗口", GetLastError());
    return added_;
}
void ControllerTray::Stop() {
    if (added_) Shell_NotifyIconW(NIM_DELETE, &icon_);
    added_ = false;
    window_ = nullptr;
}
void ControllerTray::Show() {
    ShowWindow(window_, IsIconic(window_) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(window_);
}
void ControllerTray::Menu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kControllerTrayShow, L"显示窗口");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kControllerTrayExit, L"退出");
    SetMenuDefaultItem(menu, kControllerTrayShow, FALSE);
    POINT point{}; GetCursorPos(&point);
    SetForegroundWindow(window_);
    const auto command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        point.x, point.y, 0, window_, nullptr);
    DestroyMenu(menu);
    if (command) PostMessageW(window_, WM_COMMAND, command, 0);
    PostMessageW(window_, WM_NULL, 0, 0);
}
bool ControllerTray::Handle(UINT message, WPARAM wParam, LPARAM lParam) {
    if (!window_) return false;
    if (taskbarCreated_ && message == taskbarCreated_) {
        added_ = false;
        if (!Add()) Show();
        return true;
    }
    switch (message) {
        case WM_TIMER:
            if (!added_ && GetTickCount64() >= retry_) Add();
            break;
        case WM_CLOSE:
            if (Add()) {
                ShowWindow(window_, SW_HIDE);
                LogLine("控制端窗口已隐藏到托盘，桌面连接和内网访问继续运行");
            } else {
                Show();
                MessageBoxW(window_, L"无法创建托盘图标，窗口暂时保留，请稍后重试。", L"WindowsStream", MB_ICONWARNING);
            }
            return true;
        case kControllerTrayCallback: {
            const auto event = LOWORD(lParam);
            if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP) Menu();
            else if (event == NIN_SELECT || event == NIN_KEYSELECT || event == WM_LBUTTONDBLCLK) Show();
            return true;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == kControllerTrayShow) { Show(); return true; }
            if (LOWORD(wParam) == kControllerTrayExit) {
                LogLine("用户从托盘主动退出控制端");
                DestroyWindow(window_);
                return true;
            }
            break;
        case WM_DESTROY:
            Stop();
            break;
    }
    return false;
}
