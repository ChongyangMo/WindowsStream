#include "tray.h"
#include "../../shared/host_control.h"
#include "../../shared/app_icon.h"

bool HostTray::Add(HWND owner) {
    data_ = {};
    data_.cbSize = sizeof data_; data_.hWnd = owner; data_.uID = 1;
    data_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    data_.uCallbackMessage = kTrayCallback;
    data_.hIcon = LoadAppIcon(true);
    wcscpy_s(data_.szTip, L"WindowsStream 被控端");
    added_ = Shell_NotifyIconW(NIM_ADD, &data_) != FALSE;
    if (added_) { data_.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION, &data_); }
    else LogLine("添加托盘图标失败：%lu，将保留可见窗口", GetLastError());
    return added_;
}
void HostTray::Remove() {
    if (added_) Shell_NotifyIconW(NIM_DELETE, &data_);
    added_ = false;
}
void HostTray::Status(const std::wstring& status) {
    if (!added_) return;
    const auto text = L"WindowsStream · " + status;
    wcsncpy_s(data_.szTip, text.c_str(), _TRUNCATE);
    data_.uFlags = NIF_TIP | NIF_SHOWTIP;
    Shell_NotifyIconW(NIM_MODIFY, &data_);
}
void HostTray::Menu(bool autoStart, bool keepAwake) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kTrayShow, L"显示窗口");
    AppendMenuW(menu, MF_STRING | (autoStart ? MF_CHECKED : 0), kTrayAutoStart, L"开机启动");
    AppendMenuW(menu, MF_STRING | (keepAwake ? MF_CHECKED : 0), kTrayKeepAwake, L"屏幕常亮");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kHostExitCommand, L"退出");
    POINT point{}; GetCursorPos(&point);
    SetForegroundWindow(data_.hWnd);
    const auto command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, data_.hWnd, nullptr);
    if (command) PostMessageW(data_.hWnd, WM_COMMAND, command, 0);
    PostMessageW(data_.hWnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}
