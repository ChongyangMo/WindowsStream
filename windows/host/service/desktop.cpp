#include "service.h"

namespace servermode {
namespace {
std::wstring Name(HDESK desktop) {
    wchar_t name[256]{}; DWORD size = 0;
    return desktop && GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof name, &size) ? name : L"";
}
}
DesktopBinding::~DesktopBinding() {
    if (attached_ && original_) SetThreadDesktop(original_);
    if (attached_) CloseDesktop(attached_);
}
bool DesktopBinding::Attach() {
    if (!SecureAgent()) return true;
    auto input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS |
        DESKTOP_CREATEWINDOW | DESKTOP_CREATEMENU | DESKTOP_HOOKCONTROL | DESKTOP_SWITCHDESKTOP);
    if (!input) return false;
    const auto name = Name(input);
    if (name.empty()) { CloseDesktop(input); return false; }
    if (name == Name(GetThreadDesktop(GetCurrentThreadId()))) { CloseDesktop(input); return true; }
    if (!original_) original_ = GetThreadDesktop(GetCurrentThreadId());
    if (!SetThreadDesktop(input)) { const DWORD error = GetLastError(); CloseDesktop(input); SetLastError(error); return false; }
    if (attached_) CloseDesktop(attached_);
    attached_ = input;
    LogLine("服务器线程已连接输入桌面：%s", WideToUtf8(name).c_str());
    unsigned windows = 0;
    EnumDesktopWindows(input, [](HWND window, LPARAM context) -> BOOL {
        auto& count = *reinterpret_cast<unsigned*>(context);
        if (++count > 16) return FALSE;
        wchar_t type[128]{}; GetClassNameW(window, type, 128);
        DWORD process = 0; GetWindowThreadProcessId(window, &process);
        LogLine("输入桌面窗口：进程 %lu，类 %s，可见 %u", process, WideToUtf8(type).c_str(), IsWindowVisible(window) ? 1u : 0u);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&windows));
    return true;
}
bool DesktopAccessible() {
    const auto input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!input) return false;
    const auto name = Name(input);
    CloseDesktop(input);
    return !name.empty() && (SecureAgent() ? name == Name(GetThreadDesktop(GetCurrentThreadId())) : _wcsicmp(name.c_str(), L"Default") == 0);
}
}
