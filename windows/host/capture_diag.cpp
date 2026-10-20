#include "capture_diag.h"
#include <tlhelp32.h>
#include <string>

namespace {
std::string DesktopName(HDESK desktop) {
    wchar_t name[128]{};
    DWORD needed = 0;
    if (!desktop || !GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof name, &needed)) return "无法读取";
    return WideToUtf8(name);
}

std::string ProcessPath(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return "无法读取";
    wchar_t path[MAX_PATH]{};
    DWORD size = MAX_PATH;
    const bool ok = QueryFullProcessImageNameW(process, 0, path, &size) != FALSE;
    CloseHandle(process);
    return ok ? WideToUtf8(path) : "无法读取";
}

bool StartsWith(const std::wstring& text, const std::wstring& prefix) {
    return !prefix.empty() && text.size() >= prefix.size() && _wcsnicmp(text.c_str(), prefix.c_str(), prefix.size()) == 0;
}
}

void LogDuplicationDenied() {
    // 每个进程十分钟内只详细记录一次，避免重连时刷屏。
    static ULONGLONG next = 0;
    if (GetTickCount64() < next) return;
    next = GetTickCount64() + 600000;

    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    LogLine("桌面复制诊断：本进程会话 %lu，控制台会话 %lu，远程会话 %s", session, WTSGetActiveConsoleSessionId(),
        GetSystemMetrics(SM_REMOTESESSION) ? "是" : "否");

    HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    LogLine("桌面复制诊断：输入桌面 %s，线程桌面 %s", DesktopName(input).c_str(),
        DesktopName(GetThreadDesktop(GetCurrentThreadId())).c_str());
    if (input) CloseDesktop(input);

    DWORD foreground = 0;
    if (HWND window = GetForegroundWindow()) GetWindowThreadProcessId(window, &foreground);
    LogLine("桌面复制诊断：前台程序 %s", foreground ? ProcessPath(foreground).c_str() : "无");

    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const wchar_t* selfName = wcsrchr(self, L'\\') ? wcsrchr(self, L'\\') + 1 : self;
    if (HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{sizeof entry};
        std::string others;
        for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
            if (entry.th32ProcessID != GetCurrentProcessId() && _wcsicmp(entry.szExeFile, selfName) == 0)
                others += " " + std::to_string(entry.th32ProcessID);
        CloseHandle(snapshot);
        LogLine("桌面复制诊断：其他同名进程%s", others.empty() ? " 无" : others.c_str());
    }

    // 防截屏软件通常向进程注入模块拦截 DXGI；记录系统目录和本程序目录以外的已加载模块。
    wchar_t windows[MAX_PATH]{};
    GetWindowsDirectoryW(windows, MAX_PATH);
    const std::wstring selfDir(self, static_cast<size_t>(selfName - self));
    if (HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId()); snapshot != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W module{sizeof module};
        int count = 0;
        for (BOOL more = Module32FirstW(snapshot, &module); more; more = Module32NextW(snapshot, &module)) {
            const std::wstring path = module.szExePath;
            if (StartsWith(path, windows) || StartsWith(path, selfDir)) continue;
            LogLine("桌面复制诊断：非系统模块 %s", WideToUtf8(path).c_str());
            ++count;
        }
        CloseHandle(snapshot);
        if (!count) LogLine("桌面复制诊断：未发现系统目录以外的注入模块");
    }
}
