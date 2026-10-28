#include "picker.h"
#include "../../../shared/app_icon.h"

namespace workspace {
void ConnectionPicker::Show(HWND owner, const std::wstring& base, std::function<void(const Profile&)> open) {
    if (window_) { ShowWindow(window_, SW_SHOW); SetForegroundWindow(window_); return; }
    base_ = base; open_ = std::move(open); profiles_ = LoadProfiles(base);
    WNDCLASSW cls{}; cls.lpfnWndProc = Procedure; cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"WindowsStreamConnectionPicker"; cls.hIcon = LoadAppIcon();
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&cls);
    const UINT dpi = GetDpiForWindow(owner);
    const auto px = [dpi](int n) { return MulDiv(n, static_cast<int>(dpi), 96); };
    RECT rect{0, 0, px(380), px(290)};
    AdjustWindowRectExForDpi(&rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_CONTROLPARENT, dpi);
    window_ = CreateWindowExW(WS_EX_CONTROLPARENT, cls.lpszClassName, L"新建连接", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, owner, nullptr, cls.hInstance, this);
    if (!window_) return;
    const auto child = [&](const wchar_t* type, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id = 0) {
        auto control = CreateWindowExW(type == std::wstring(L"EDIT") ? WS_EX_CLIENTEDGE : 0, type, text, WS_CHILD | WS_VISIBLE | style,
            px(x), px(y), px(w), px(h), window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), cls.hInstance, nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        return control;
    };
    list_ = child(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 20, 18, 340, 200, 10);
    SendMessageW(list_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"新连接"));
    for (const auto& profile : profiles_) SendMessageW(list_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(profile.name.c_str()));
    SendMessageW(list_, CB_SETCURSEL, 0, 0);
    child(L"STATIC", L"地址", 0, 20, 65, 52, 24);
    host_ = child(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 78, 60, 282, 28);
    child(L"STATIC", L"端口", 0, 20, 108, 52, 24);
    port_ = child(L"EDIT", L"7400", WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, 78, 103, 282, 28);
    child(L"STATIC", L"口令", 0, 20, 151, 52, 24);
    token_ = child(L"EDIT", ReadSetting(base_, L"connection", L"token").c_str(), WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL, 78, 146, 282, 28);
    SendMessageW(host_, EM_SETLIMITTEXT, 253, 0); SendMessageW(port_, EM_SETLIMITTEXT, 5, 0); SendMessageW(token_, EM_SETLIMITTEXT, 511, 0);
    error_ = child(L"STATIC", L"", 0, 20, 189, 340, 36);
    child(L"BUTTON", L"连接", WS_TABSTOP | BS_DEFPUSHBUTTON, 260, 239, 100, 30, IDOK);
    ShowWindow(window_, SW_SHOW); SetFocus(host_);
}
void ConnectionPicker::Select() {
    const auto index = SendMessageW(list_, CB_GETCURSEL, 0, 0);
    if (index <= 0 || static_cast<size_t>(index) > profiles_.size()) {
        SetWindowTextW(host_, L""); SetWindowTextW(port_, L"7400");
        SetWindowTextW(token_, ReadSetting(base_, L"connection", L"token").c_str());
        return;
    }
    const auto& profile = profiles_[static_cast<size_t>(index) - 1];
    SetWindowTextW(host_, ReadSetting(profile.path, L"connection", L"host").c_str());
    SetWindowTextW(port_, ReadSetting(profile.path, L"connection", L"port", L"7400").c_str());
    SetWindowTextW(token_, ReadSetting(profile.path, L"connection", L"token").c_str());
    SetWindowTextW(error_, L"");
}
void ConnectionPicker::Connect() {
    wchar_t host[256]{}, port[8]{}, token[512]{};
    GetWindowTextW(host_, host, 256); GetWindowTextW(port_, port, 8); GetWindowTextW(token_, token, 512);
    wchar_t* end = nullptr; const auto number = wcstoul(port, &end, 10);
    Profile profile; std::wstring error;
    if (!*port || *end || !SaveProfile(base_, host, static_cast<unsigned>(number), token, profile, error)) {
        SetWindowTextW(error_, error.empty() ? L"请输入有效端口" : error.c_str()); return;
    }
    open_(profile); Close();
}
void ConnectionPicker::Close() { if (window_) DestroyWindow(window_); window_ = nullptr; }
bool ConnectionPicker::Translate(MSG& message) { return window_ && IsDialogMessageW(window_, &message); }
LRESULT CALLBACK ConnectionPicker::Procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto self = reinterpret_cast<ConnectionPicker*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<ConnectionPicker*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self) {
        if (message == WM_COMMAND && LOWORD(wParam) == 10 && HIWORD(wParam) == CBN_SELCHANGE) { self->Select(); return 0; }
        if (message == WM_COMMAND && LOWORD(wParam) == IDOK) { self->Connect(); return 0; }
        if (message == WM_COMMAND && LOWORD(wParam) == IDCANCEL || message == WM_CLOSE) { self->Close(); return 0; }
        if (message == WM_NCDESTROY) self->window_ = nullptr;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
}
