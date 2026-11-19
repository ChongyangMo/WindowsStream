#include "keepalive_task.h"
#include "keepalive.h"
#include "autorun.h"
#include "../config.h"
#include "../../shared/host_control.h"
#include "../../shared/update_package.h"
#include <wtsapi32.h>

HRESULT RegisterKeepAliveTask(const std::filesystem::path& program, const std::filesystem::path& root, bool elevated);

namespace {
constexpr DWORD kCheckInterval = 10000;

struct SessionState {
    std::filesystem::path path;
    std::wstring section, stamp;

    explicit SessionState(const std::filesystem::path& root) : path(root / L".windowsstream-runtime" / L"scheduled-state.ini") {
        DWORD session = 0, bytes = 0;
        LPWSTR data = nullptr;
        if (!ProcessIdToSessionId(GetCurrentProcessId(), &session) || !session ||
            !WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSSessionInfo, &data, &bytes)) return;
        if (bytes >= sizeof(WTSINFOW)) {
            const auto logon = reinterpret_cast<const WTSINFOW*>(data)->LogonTime.QuadPart;
            if (logon > 0) {
                section = L"session_" + std::to_wstring(session);
                stamp = std::to_wstring(logon);
            }
        }
        WTSFreeMemory(data);
    }

    bool Save(bool active) const {
        return !stamp.empty() && WritePrivateProfileStringW(section.c_str(), L"state",
            (active ? stamp : L"-" + stamp).c_str(), path.c_str());
    }

    bool AllowsStart(const HostConfig& config) const {
        if (stamp.empty()) return false;
        wchar_t state[32]{};
        GetPrivateProfileStringW(section.c_str(), L"state", L"", state, _countof(state), path.c_str());
        if (L"-" + stamp == state) return false;
        return stamp == state || (config.manageAutoStart && config.autoStart);
    }
};

bool HostIsMissing() {
    HANDLE active = OpenMutexW(SYNCHRONIZE, FALSE, kHostInstanceMutex);
    if (!active) return GetLastError() == ERROR_FILE_NOT_FOUND;
    CloseHandle(active);
    return false;
}

bool LaunchHost(const std::filesystem::path& target) {
    auto command = L"\"" + target.wstring() + L"\" --background";
    STARTUPINFOW startup{sizeof startup};
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(target.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, target.parent_path().c_str(), &startup, &child)) {
        LogLine("计划任务恢复被控端失败：%lu", GetLastError());
        return false;
    }
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    return true;
}
}

void StartScheduledKeepAlive(const std::filesystem::path& root, bool elevated) {
    const auto program = PrepareKeepAliveProgram();
    if (program.empty() || !SessionState(root).Save(true)) {
        LogLine("无法准备被控端计划保活，请检查运行目录权限和登录会话");
        return;
    }
    if (const auto error = RegisterKeepAliveTask(program, root, elevated && IsAdminPackageRuntime(root)); FAILED(error))
        LogLine("启动被控端计划保活失败：0x%08lX", static_cast<unsigned long>(error));
}

void PauseScheduledKeepAlive(const std::filesystem::path& root) {
    const SessionState state(root);
    std::error_code error;
    if (std::filesystem::is_regular_file(state.path, error) && !state.Save(false))
        LogLine("无法暂停当前会话的被控端计划保活");
}

int RunScheduledKeepAlive() {
    const auto program = std::filesystem::path(updates::ModulePath());
    const auto runtime = program.parent_path().parent_path();
    const auto root = runtime.parent_path();
    const auto target = root / L"WindowsStreamHost.exe";
    const auto ini = root / L"host.ini";
    if (program.filename() != L"WindowsStreamKeepAlive.exe" || runtime.filename() != L".windowsstream-runtime") return 2;
    const SessionState state(root);
    if (state.stamp.empty()) return 2;
    SetRuntimeDirectory(root.wstring());
    bool announced = false;
    ULONGLONG nextAttempt = 0;
    for (;; Sleep(kCheckInterval)) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(target, error) || !std::filesystem::is_regular_file(ini, error)) return 0;
        HostConfig config;
        ReadHostAutoStart(ini, config);
        if (!state.AllowsStart(config)) return 0;
        if (!announced) {
            LogLine("被控端计划保活已启动，每 10 秒检查一次");
            announced = true;
        }
        if (GetTickCount64() < nextAttempt) continue;
        const HostUpdateLock updateLock(0);
        if (!updateLock || !HostIsMissing() || !state.AllowsStart(config)) continue;
        if (!TakeRecoveryAttempt(root)) {
            nextAttempt = GetTickCount64() + 60000;
            continue;
        }
        if (config.relayAutoStart ? LaunchRelayAutoStart() == 0 : LaunchHost(target))
            LogLine("计划保活检测到被控端已停止，已请求重新启动");
    }
}
