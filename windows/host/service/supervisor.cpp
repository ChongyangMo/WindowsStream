#include "service.h"

namespace servermode {
namespace {
constexpr wchar_t kName[] = L"WindowsStreamServer";
SERVICE_STATUS_HANDLE statusHandle = nullptr;
HANDLE stop = nullptr;
SERVICE_STATUS state{};
void Status(DWORD value, DWORD error = NO_ERROR) {
    state.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    state.dwCurrentState = value;
    state.dwControlsAccepted = value == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    state.dwWin32ExitCode = error;
    state.dwWaitHint = value == SERVICE_STOP_PENDING ? 5000 : 0;
    SetServiceStatus(statusHandle, &state);
}
DWORD WINAPI Control(DWORD control, DWORD, void*, void*) {
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        Status(SERVICE_STOP_PENDING); SetEvent(stop); return NO_ERROR;
    }
    if (control == SERVICE_CONTROL_INTERROGATE) return NO_ERROR;
    return ERROR_CALL_NOT_IMPLEMENTED;
}
void WINAPI Main(DWORD, wchar_t**) {
    statusHandle = RegisterServiceCtrlHandlerExW(kName, Control, nullptr);
    if (!statusHandle) return;
    stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop || !LocalSystem()) { Status(SERVICE_STOPPED, ERROR_ACCESS_DENIED); if (stop) CloseHandle(stop); return; }
    const auto job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits)) {
        Status(SERVICE_STOPPED, GetLastError()); if (job) CloseHandle(job); CloseHandle(stop); return;
    }
    Status(SERVICE_RUNNING);
    PROCESS_INFORMATION child{}; DWORD active = 0xffffffff, lastError = 0;
    while (WaitForSingleObject(stop, 1000) == WAIT_TIMEOUT) {
        const DWORD session = InteractiveSession();
        if (child.hProcess && (session != active || WaitForSingleObject(child.hProcess, 0) != WAIT_TIMEOUT)) {
            TerminateProcess(child.hProcess, 0); WaitForSingleObject(child.hProcess, 5000);
            CloseHandle(child.hProcess); CloseHandle(child.hThread); child = {};
        }
        if (child.hProcess || session == 0xffffffff) continue;
        if (LaunchAgent(session, child)) {
            if (!AssignProcessToJobObject(job, child.hProcess)) {
                const DWORD error = GetLastError(); TerminateProcess(child.hProcess, error);
                CloseHandle(child.hProcess); CloseHandle(child.hThread); child = {};
                LogLine("服务器会话代理无法加入退出回收组：%lu", error); continue;
            }
            ResumeThread(child.hThread);
            active = session; lastError = 0;
            LogLine("服务器服务已启动会话代理：会话 %lu，进程 %lu", active, child.dwProcessId);
        } else {
            const DWORD error = GetLastError();
            if (error != lastError) LogLine("服务器服务启动会话代理失败：%lu", error);
            lastError = error;
            WaitForSingleObject(stop, 2000);
        }
    }
    CloseHandle(job);
    if (child.hProcess) {
        WaitForSingleObject(child.hProcess, 5000);
        CloseHandle(child.hProcess); CloseHandle(child.hThread);
    }
    LogLine("服务器服务已停止，交互会话代理已回收");
    CloseHandle(stop); stop = nullptr; Status(SERVICE_STOPPED);
}
}
int RunService() {
    SERVICE_TABLE_ENTRYW table[]{{const_cast<wchar_t*>(kName), Main}, {nullptr, nullptr}};
    return StartServiceCtrlDispatcherW(table) ? 0 : static_cast<int>(GetLastError());
}
}
