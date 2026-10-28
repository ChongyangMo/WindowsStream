#include "service.h"

namespace servermode {
namespace {
std::wstring Name(HDESK desktop) {
    wchar_t name[256]{}; DWORD size = 0;
    return desktop && GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof name, &size) ? name : L"";
}
}
bool DesktopBinding::Attach() {
    if (!SecureAgent()) return true;
    auto input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS |
        DESKTOP_CREATEWINDOW | DESKTOP_CREATEMENU | DESKTOP_HOOKCONTROL | DESKTOP_SWITCHDESKTOP);
    if (!input) return false;
    const auto name = Name(input);
    if (name.empty()) { CloseDesktop(input); return false; }
    if (name == Name(GetThreadDesktop(GetCurrentThreadId()))) { CloseDesktop(input); return true; }
    CloseDesktop(input);
    if (name != L"Default" && name != L"Winlogon") { SetLastError(ERROR_NOT_SUPPORTED); return false; }
    DWORD session = 0; ProcessIdToSessionId(GetCurrentProcessId(), &session);
    const auto section = L"session." + std::to_wstring(session);
    const auto ini = PortableDir() + L"\\service-runtime.ini";
    if (!WritePrivateProfileStringW(section.c_str(), L"desktop", name.c_str(), ini.c_str())) return false;
    // 安全桌面需要进程从该桌面启动；仅切换线程仍可能得到背景或被拒绝输入。
    LogLine("服务器输入桌面已切换为 %s，请求系统服务重建会话代理", WideToUtf8(name).c_str());
    SetLastError(ERROR_RETRY);
    return false;
}
bool DesktopAccessible() {
    const auto input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!input) return false;
    const auto name = Name(input);
    CloseDesktop(input);
    return !name.empty() && (SecureAgent() ? name == Name(GetThreadDesktop(GetCurrentThreadId())) : _wcsicmp(name.c_str(), L"Default") == 0);
}
}
