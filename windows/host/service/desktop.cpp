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
