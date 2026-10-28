#include "service.h"
#include <wtsapi32.h>
#include <tlhelp32.h>

namespace servermode {
namespace {
bool Enable(HANDLE token, const wchar_t* name) {
    TOKEN_PRIVILEGES privilege{}; privilege.PrivilegeCount = 1;
    if (!LookupPrivilegeValueW(nullptr, name, &privilege.Privileges[0].Luid)) return false;
    privilege.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    SetLastError(ERROR_SUCCESS);
    return AdjustTokenPrivileges(token, FALSE, &privilege, 0, nullptr, nullptr) && GetLastError() == ERROR_SUCCESS;
}
HANDLE SessionToken(DWORD session) {
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return nullptr;
    PROCESSENTRY32W entry{sizeof entry}; HANDLE token = nullptr;
    if (Process32FirstW(snapshot, &entry)) do {
        DWORD candidate = 0;
        if (_wcsicmp(entry.szExeFile, L"winlogon.exe") || !ProcessIdToSessionId(entry.th32ProcessID, &candidate) || candidate != session) continue;
        const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (process) { OpenProcessToken(process, TOKEN_DUPLICATE | TOKEN_QUERY, &token); CloseHandle(process); }
        break;
    } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    return token;
}
}
DWORD InteractiveSession() {
    WTS_SESSION_INFOW* sessions = nullptr; DWORD count = 0;
    DWORD selected = 0xffffffff;
    if (WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) {
        const DWORD console = WTSGetActiveConsoleSessionId();
        for (DWORD i = 0; i < count; ++i) {
            if (sessions[i].SessionId && sessions[i].State == WTSActive) {
                selected = sessions[i].SessionId;
                if (selected == console) break;
            }
        }
        WTSFreeMemory(sessions);
    }
    if (selected == 0xffffffff) selected = WTSGetActiveConsoleSessionId();
    return selected == 0 ? 0xffffffff : selected;
}
bool LaunchAgent(DWORD session, const std::wstring& attentionEvent, PROCESS_INFORMATION& process) {
    HANDLE source = nullptr, token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, &source)) return false;
    const bool privileges = Enable(source, SE_TCB_NAME) && Enable(source, SE_ASSIGNPRIMARYTOKEN_NAME) && Enable(source, SE_INCREASE_QUOTA_NAME);
    Enable(source, SE_DEBUG_NAME);
    const auto sessionToken = privileges ? SessionToken(session) : nullptr;
    bool ok = privileges && sessionToken && DuplicateTokenEx(sessionToken, TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenPrimary, &token);
    DWORD error = GetLastError();
    if (sessionToken) CloseHandle(sessionToken);
    CloseHandle(source);
    if (ok) {
        ok = SetTokenInformation(token, TokenSessionId, &session, sizeof session) != FALSE;
        DWORD uiAccess = TRUE;
        if (ok) ok = SetTokenInformation(token, TokenUIAccess, &uiAccess, sizeof uiAccess) != FALSE;
        if (ok) {
            wchar_t module[MAX_PATH]{};
            const DWORD size = GetModuleFileNameW(nullptr, module, MAX_PATH);
            if (!size || size >= MAX_PATH) ok = false;
            else {
                std::wstring command = L"\"" + std::wstring(module) + L"\" --server-agent \"" + attentionEvent + L"\"";
                wchar_t desktop[32]{};
                const auto section = L"session." + std::to_wstring(session);
                const auto runtime = PortableDir() + L"\\service-runtime.ini";
                GetPrivateProfileStringW(section.c_str(), L"desktop", L"Winlogon", desktop, 32, runtime.c_str());
                if (std::wstring(desktop) != L"Default" && std::wstring(desktop) != L"Winlogon") wcscpy_s(desktop, L"Winlogon");
                std::wstring station = L"winsta0\\" + std::wstring(desktop);
                STARTUPINFOW startup{sizeof startup}; startup.lpDesktop = station.data();
                LogLine("服务器会话代理启动桌面：%s，会话 %lu", WideToUtf8(station).c_str(), session);
                ok = CreateProcessAsUserW(token, module, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                    nullptr, PortableDir().c_str(), &startup, &process) != FALSE;
            }
        }
        error = GetLastError();
        CloseHandle(token);
    }
    if (!ok) SetLastError(error);
    return ok;
}
}
