#include "window.h"
#include "../../../shared/app_icon.h"
#include <windowsx.h>
#include <set>

namespace workspace {
std::vector<Window*> windows;

HWND Window::Create(int show) {
    WNDCLASSEXW cls{sizeof cls}; cls.lpfnWndProc = Procedure; cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = kWindowClass; cls.hIcon = LoadAppIcon(); cls.hIconSm = LoadAppIcon(true);
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassExW(&cls);
    window_ = CreateWindowExW(WS_EX_APPWINDOW, kWindowClass, L"WindowsStream", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 820, nullptr, nullptr, cls.hInstance, this);
    if (!window_) return nullptr;
    windows.push_back(this);
    tabsControl_ = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_OWNERDRAWFIXED | TCS_FIXEDWIDTH | TCS_FOCUSNEVER,
        0, 0, 1, 1, window_, nullptr, cls.hInstance, nullptr);
    add_ = CreateWindowW(L"BUTTON", L"+", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 1, 1, window_, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kWorkspaceNew)), cls.hInstance, nullptr);
    for (const auto child : {tabsControl_, add_}) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    SetWindowSubclass(tabsControl_, TabProcedure, 1, reinterpret_cast<DWORD_PTR>(this));
    SetTimer(window_, 1, 150, nullptr);
    ShowWindow(window_, show == SW_HIDE ? SW_SHOWNORMAL : show); Layout();
    return window_;
}
int Window::HeaderHeight() const { return MulDiv(38, static_cast<int>(GetDpiForWindow(window_)), 96); }
void Window::Refresh() {
    TabCtrl_DeleteAllItems(tabsControl_);
    for (size_t i = 0; i < tabs_.size(); ++i) {
        TCITEMW item{}; item.mask = TCIF_TEXT; item.pszText = tabs_[i].profile.name.data();
        TabCtrl_InsertItem(tabsControl_, static_cast<int>(i), &item);
    }
    TabCtrl_SetCurSel(tabsControl_, selected_); Layout();
    HMENU menu = nullptr;
    for (const auto& tab : tabs_) if (tab.profile.primary && tab.menu) { menu = tab.menu; break; }
    if (GetMenu(window_) != menu) SetMenu(window_, menu);
    DrawMenuBar(window_);
}
void Window::Layout() {
    if (!tabsControl_) return;
    RECT area{}; GetClientRect(window_, &area);
    const int height = HeaderHeight(), button = height - 6;
    const int width = std::max(0L, area.right);
    const int tabWidth = std::clamp((width - height) / std::max(1, static_cast<int>(tabs_.size())), height * 2, height * 6);
    TabCtrl_SetItemSize(tabsControl_, tabWidth, height - 5);
    const int stripWidth = std::max(1, std::min(width - height, tabWidth * static_cast<int>(tabs_.size()) + 6));
    SetWindowPos(tabsControl_, nullptr, 0, 0, stripWidth, height, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(add_, nullptr, stripWidth + 2, 2, button, button, SWP_NOZORDER | SWP_NOACTIVATE);
    for (size_t i = 0; i < tabs_.size(); ++i) {
        const auto view = tabs_[i].view;
        if (!view || !IsWindow(view)) continue;
        SetWindowPos(view, nullptr, 0, height, width, std::max(1L, area.bottom - height), SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindowAsync(view, static_cast<int>(i) == selected_ ? SW_SHOWNA : SW_HIDE);
    }
}
void Window::Select(int index) {
    if (index < 0 || static_cast<size_t>(index) >= tabs_.size()) return;
    selected_ = index; TabCtrl_SetCurSel(tabsControl_, index); Layout();
    const auto view = tabs_[static_cast<size_t>(index)].view;
    if (view) {
        PostMessageW(view, kViewerFocus, 0, 0);
        // 旧版连接也可以放入标签页；无需重启正在承担代理的进程。
        if (tabs_[static_cast<size_t>(index)].borrowed) {
            const auto thread = GetWindowThreadProcessId(view, nullptr);
            if (AttachThreadInput(GetCurrentThreadId(), thread, TRUE)) {
                SetFocus(view); AttachThreadInput(GetCurrentThreadId(), thread, FALSE);
            }
        }
    }
}
void Window::NewConnection() { picker_.Show(window_, base_, [this](const Profile& profile) { Open(profile); }); }
bool Window::Translate(MSG& message) {
    if (picker_.Translate(message)) return true;
    if (message.message != WM_KEYDOWN || GetAncestor(message.hwnd, GA_ROOT) != window_) return false;
    if (!(GetKeyState(VK_CONTROL) & 0x8000)) return false;
    UINT command = message.wParam == 'T' || message.wParam == 'N' ? kWorkspaceNew :
        message.wParam == 'W' ? kWorkspaceClose : message.wParam == VK_TAB ? kWorkspaceNext : 0;
    if (!command) return false;
    SendMessageW(window_, kWorkspaceAction, command, (GetKeyState(VK_SHIFT) & 0x8000) != 0); return true;
}
LRESULT CALLBACK Window::Keyboard(int code, WPARAM message, LPARAM data) {
    static std::set<DWORD> consumed;
    if (code == HC_ACTION) {
        const auto key = reinterpret_cast<KBDLLHOOKSTRUCT*>(data)->vkCode;
        const bool up = message == WM_KEYUP || message == WM_SYSKEYUP;
        if (key == VK_LWIN || key == VK_RWIN) for (const auto workspace : windows) {
            if (GetForegroundWindow() != workspace->window_ || workspace->selected_ < 0 || static_cast<size_t>(workspace->selected_) >= workspace->tabs_.size()) continue;
            const auto& tab = workspace->tabs_[static_cast<size_t>(workspace->selected_)];
            if (tab.borrowed && tab.view) { PostMessageW(tab.view, WM_APP + 4, !up, key == VK_RWIN); return 1; }
        }
        if (up && consumed.erase(key)) return 1;
        if (!up && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && !(GetAsyncKeyState(VK_MENU) & 0x8000)) {
            const UINT command = key == 'T' || key == 'N' ? kWorkspaceNew : key == 'W' ? kWorkspaceClose : key == VK_TAB ? kWorkspaceNext : 0;
            if (command) for (auto workspace : windows) if (GetForegroundWindow() == workspace->window_) {
                if (workspace->selected_ >= 0 && static_cast<size_t>(workspace->selected_) < workspace->tabs_.size())
                    PostMessageW(workspace->tabs_[static_cast<size_t>(workspace->selected_)].view, WM_KILLFOCUS, 0, 0);
                if (consumed.insert(key).second) PostMessageW(workspace->window_, kWorkspaceAction, command, (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0);
                return 1;
            }
        }
    }
    return CallNextHookEx(nullptr, code, message, data);
}
void Window::Fullscreen() {
    const auto style = GetWindowLongPtrW(window_, GWL_STYLE);
    if (!fullscreen_) {
        MONITORINFO monitor{sizeof monitor};
        if (!GetWindowPlacement(window_, &placement_) || !GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor)) return;
        SetWindowLongPtrW(window_, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(window_, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
            monitor.rcMonitor.right - monitor.rcMonitor.left, monitor.rcMonitor.bottom - monitor.rcMonitor.top, SWP_FRAMECHANGED);
    } else {
        SetWindowLongPtrW(window_, GWL_STYLE, style | WS_OVERLAPPEDWINDOW); SetWindowPlacement(window_, &placement_);
        SetWindowPos(window_, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    }
    fullscreen_ = !fullscreen_;
}
void Window::DrawTab(const DRAWITEMSTRUCT& item) {
    const auto index = static_cast<size_t>(item.itemID); if (index >= tabs_.size()) return;
    const COLORREF background = static_cast<int>(index) == selected_ ? RGB(255, 255, 255) : RGB(237, 239, 242);
    const HBRUSH brush = CreateSolidBrush(background); FillRect(item.hDC, &item.rcItem, brush); DeleteObject(brush);
    SetBkMode(item.hDC, TRANSPARENT); SetTextColor(item.hDC, RGB(35, 38, 42));
    RECT text = item.rcItem; const int padding = HeaderHeight() / 3; text.left += padding; text.right -= padding * 3;
    DrawTextW(item.hDC, tabs_[index].profile.name.c_str(), -1, &text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    const int x = item.rcItem.right - padding * 2, y = (item.rcItem.top + item.rcItem.bottom) / 2;
    const int radius = std::max(3, padding / 3);
    MoveToEx(item.hDC, x - radius, y - radius, nullptr); LineTo(item.hDC, x + radius, y + radius);
    MoveToEx(item.hDC, x + radius, y - radius, nullptr); LineTo(item.hDC, x - radius, y + radius);
}
void Window::Menu(int index) {
    if (index < 0) return;
    HMENU menu = CreatePopupMenu(); if (!menu) return;
    AppendMenuW(menu, MF_STRING | (tabs_[static_cast<size_t>(index)].view ? 0 : MF_GRAYED), kWorkspaceDetach, L"移到新窗口");
    AppendMenuW(menu, MF_STRING, kWorkspaceClose, L"关闭标签页");
    if (ReadSetting(tabs_[static_cast<size_t>(index)].profile.path, L"permissions", L"secure_attention", L"0") == L"1")
        AppendMenuW(menu, MF_STRING, kWorkspaceSecureAttention, L"Ctrl+Alt+Del");
    POINT point{}; GetCursorPos(&point);
    const auto command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, window_, nullptr);
    DestroyMenu(menu);
    if (command == kWorkspaceDetach) Detach(index);
    else if (command == kWorkspaceClose) CloseTab(index);
    else if (command == kWorkspaceSecureAttention && tabs_[static_cast<size_t>(index)].view)
        PostMessageW(tabs_[static_cast<size_t>(index)].view, kViewerSecureAttention, 0, 0);
}
LRESULT CALLBACK Window::TabProcedure(HWND control, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data) {
    auto self = reinterpret_cast<Window*>(data);
    if (message == WM_LBUTTONDOWN || message == WM_RBUTTONUP || message == WM_MBUTTONUP) {
        TCHITTESTINFO hit{}; hit.pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const int index = TabCtrl_HitTest(control, &hit);
        if (index >= 0) {
            RECT rect{}; TabCtrl_GetItemRect(control, index, &rect);
            if (message == WM_RBUTTONUP) { self->Menu(index); return 0; }
            if (message == WM_MBUTTONUP || hit.pt.x >= rect.right - self->HeaderHeight() * 3 / 4) { self->CloseTab(index); return 0; }
        }
    }
    return DefSubclassProc(control, message, wParam, lParam);
}
LRESULT CALLBACK Window::Procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto self = reinterpret_cast<Window*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); self->window_ = window;
    }
    if (!self) return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
        case WM_SIZE: self->Layout(); return 0;
        case WM_DPICHANGED: {
            const auto rect = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
            self->Layout(); return 0;
        }
        case WM_GETMINMAXINFO: reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = {420, 280}; return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == kWorkspaceNew) { self->NewConnection(); return 0; }
            for (const auto& tab : self->tabs_) if (tab.profile.primary && tab.menu && tab.view) {
                PostMessageW(tab.view, message, wParam, lParam); return 0;
            }
            break;
        case WM_NOTIFY: if (reinterpret_cast<NMHDR*>(lParam)->hwndFrom == self->tabsControl_ && reinterpret_cast<NMHDR*>(lParam)->code == TCN_SELCHANGE) {
            self->Select(TabCtrl_GetCurSel(self->tabsControl_)); return 0;
        } break;
        case WM_DRAWITEM: self->DrawTab(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam)); return TRUE;
        case WM_TIMER: self->Poll(); return 0;
        case WM_SETFOCUS: self->Select(self->selected_); return 0;
        case kWorkspaceAction:
            if (wParam == kWorkspaceNew) self->NewConnection();
            else if (wParam == kWorkspaceClose) self->CloseTab(self->selected_);
            else if (wParam == kWorkspaceNext && !self->tabs_.empty()) self->Select((self->selected_ + (lParam ? static_cast<int>(self->tabs_.size()) - 1 : 1)) % static_cast<int>(self->tabs_.size()));
            else if (wParam == kWorkspaceFullscreen) self->Fullscreen();
            return 0;
        case WM_COPYDATA: {
            const auto data = reinterpret_cast<COPYDATASTRUCT*>(lParam);
            if (data->dwData < 1 || data->dwData > 3 || data->cbData < sizeof(wchar_t) || data->cbData > 65536 || data->cbData % sizeof(wchar_t)) return FALSE;
            const auto text = static_cast<const wchar_t*>(data->lpData);
            if (!text || text[data->cbData / sizeof(wchar_t) - 1] != L'\0') return FALSE;
            const std::wstring path = text;
            if (data->dwData != 1) {
                for (const auto frame : windows) for (size_t i = 0; i < frame->tabs_.size(); ++i) {
                    if (CanonicalPath(frame->tabs_[i].profile.path) == CanonicalPath(path)) {
                        if (data->dwData == 2) frame->Detach(static_cast<int>(i)); else frame->CloseTab(static_cast<int>(i));
                        return TRUE;
                    }
                }
                return FALSE;
            }
            if (path.empty()) self->NewConnection();
            else if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
                self->Open({ReadSetting(path, L"connection", L"name", L"WindowsStream"), path,
                    CanonicalPath(path) == CanonicalPath(self->base_) && LoadProfiles(self->base_).front().primary});
            ShowWindow(window, SW_RESTORE); SetForegroundWindow(window); return TRUE;
        }
        case WM_CLOSE: DestroyWindow(window); return 0;
        case WM_DESTROY:
            self->picker_.Close();
            SetMenu(window, nullptr);
            for (auto& tab : self->tabs_) self->Release(tab);
            self->tabs_.clear();
            std::erase(windows, self); if (windows.empty()) PostQuitMessage(0);
            return 0;
        case WM_NCDESTROY: SetWindowLongPtrW(window, GWLP_USERDATA, 0); delete self; return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
}
