#include "keepalive.h"
#include "awake.h"
#include "lock_policy.h"
#include "../../shared/update_package.h"
#include "../../shared/host_control.h"

namespace {
void RecoveryLog(const std::filesystem::path& root, const std::string& message) {
    SYSTEMTIME time{}; GetLocalTime(&time);
    char timestamp[40]{};
    snprintf(timestamp, sizeof timestamp, "%04u-%02u-%02u %02u:%02u:%02u.%03u ", time.wYear, time.wMonth,
        time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    const auto line = std::string(timestamp) + message + "\r\n";
    HANDLE file = CreateFileW((root / L"host.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr); CloseHandle(file);
    }
}
DWORD RecoveryDelay(const std::filesystem::path& root) {
    constexpr ULONGLONG window = 10 * 60 * 1000;
    const auto path = root / L".windowsstream-runtime" / L"recovery-state.ini";
    wchar_t value[32]{};
    GetPrivateProfileStringW(L"recovery", L"window_start", L"0", value, 32, path.c_str());
    const uint64_t start = _wcstoui64(value, nullptr, 10), now = GetTickCount64();
    auto count = GetPrivateProfileIntW(L"recovery", L"attempts", 0, path.c_str());
    if (!start || now < start || now - start >= window) {
        count = 0;
        if (!WritePrivateProfileStringW(L"recovery", L"window_start", std::to_wstring(now).c_str(), path.c_str())) return INFINITE;
    }
    if (count >= 3) return static_cast<DWORD>(window - (now - start));
    return WritePrivateProfileStringW(L"recovery", L"attempts", std::to_wstring(count + 1).c_str(), path.c_str()) ? 0 : INFINITE;
}

bool Enabled(const std::filesystem::path& ini) {
    return GetPrivateProfileIntW(L"background", L"watchdog", 1, ini.c_str()) != 0;
}

ULONGLONG AwakeMilliseconds() {
    ULONGLONG ticks = 0;
    QueryUnbiasedInterruptTime(&ticks);
    return ticks / 10000;
}

DWORD WaitForParent(const std::filesystem::path& root, const std::filesystem::path& ini,
                    HANDLE parent, HANDLE stop, HANDLE uiBeat, HANDLE workerBeat) {
    const auto timeout = 1000ull * std::clamp(GetPrivateProfileIntW(L"background", L"watchdog_timeout_seconds", 120, ini.c_str()), 30u, 600u);
    ULONGLONG lastUi = AwakeMilliseconds(), lastWorker = lastUi, nextTry = 0;
    const HANDLE waited[]{stop, parent};
    DWORD signal = WAIT_TIMEOUT;
    while ((signal = WaitForMultipleObjects(2, waited, FALSE, 2000)) == WAIT_TIMEOUT) {
        const auto now = AwakeMilliseconds();
        if (WaitForSingleObject(uiBeat, 0) == WAIT_OBJECT_0) lastUi = now;
        if (WaitForSingleObject(workerBeat, 0) == WAIT_OBJECT_0) lastWorker = now;
        const bool uiStalled = now - lastUi >= timeout;
        if ((!uiStalled && now - lastWorker < timeout) || now < nextTry) continue;
        if (!Enabled(ini)) { lastUi = lastWorker = now; continue; }
        const HostUpdateLock updateLock(0);
        if (!updateLock) continue;
        // 退出和及时到达的心跳优先，避免守护重启正在正常退出的进程。
        if (WaitForMultipleObjects(2, waited, FALSE, 0) != WAIT_TIMEOUT) continue;
        if (WaitForSingleObject(uiStalled ? uiBeat : workerBeat, 0) == WAIT_OBJECT_0) {
            if (uiStalled) lastUi = now;
            else lastWorker = now;
            continue;
        }
        if (TerminateProcess(parent, ERROR_TIMEOUT)) {
            RecoveryLog(root, uiStalled ? "界面线程长时间无响应，正在恢复被控端" : "视频或连接线程长时间无响应，正在恢复被控端");
        } else {
            RecoveryLog(root, "结束无响应的被控端失败，系统错误 " + std::to_string(GetLastError()));
        }
        nextTry = now + 30000;
    }
    return signal;
}

void RestoreLeases(const std::filesystem::path& root, DWORD parentId, uint64_t created) {
    if (!RestoreScreenSaver(root, parentId, created)) RecoveryLog(root, "恢复原屏保状态失败，保留恢复记录");
    if (!RestoreLockPolicy(root, parentId, created)) RecoveryLog(root, "恢复原锁屏设置失败，保留恢复记录");
}
}

int RunRecovery(DWORD parentId, uint64_t created, HANDLE parent, HANDLE stop, HANDLE uiBeat, HANDLE workerBeat) {
    struct Handles {
        HANDLE values[4];
        ~Handles() { for (auto handle : values) if (handle) CloseHandle(handle); }
    } handles{{parent, stop, uiBeat, workerBeat}};
    const auto module = std::filesystem::path(updates::ModulePath());
    const auto hash = WideToUtf8(module.parent_path().filename().wstring());
    const auto root = module.parent_path().parent_path().parent_path();
    if (!parent || !stop || !uiBeat || !workerBeat || module.filename() != L"WindowsStreamKeepAlive.exe" ||
        module.parent_path().parent_path().filename() != L".windowsstream-runtime" || hash.size() != 64 ||
        hash.find_first_not_of("0123456789abcdef") != std::string::npos || GetProcessId(parent) != parentId ||
        !created || ProcessCreation(parent) != created) return 2;
    std::wstring path(32768, L'\0'); DWORD length = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(parent, 0, path.data(), &length)) return 2;
    path.resize(length);
    std::error_code error;
    const auto target = root / L"WindowsStreamHost.exe";
    if (!std::filesystem::equivalent(target, path, error) || error) return 2;
    const auto ini = root / L"host.ini";
    const auto result = WaitForParent(root, ini, parent, stop, uiBeat, workerBeat);
    if (result != WAIT_OBJECT_0 + 1) return 0;
    RestoreLeases(root, parentId, created);
    DWORD code = 0;
    if (!GetExitCodeProcess(parent, &code) || code == 0 || !Enabled(ini)) return 0;
    RecoveryLog(root, "检测到被控端异常退出，退出码 " + std::to_string(code) + "，准备恢复");
    while (WaitForSingleObject(stop, 2000) == WAIT_TIMEOUT && Enabled(ini)) {
        DWORD delay = 0;
        {
            const HostUpdateLock updateLock(0);
            if (!updateLock) continue;
            HANDLE active = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\WindowsStreamHost");
            if (active) { CloseHandle(active); return 0; }
            delay = RecoveryDelay(root);
            if (!delay) {
                std::wstring command = L"\"" + target.wstring() + L"\" --background";
                STARTUPINFOW startup{sizeof startup};
                startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
                PROCESS_INFORMATION child{};
                if (CreateProcessW(target.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                                   root.c_str(), &startup, &child)) {
                    CloseHandle(child.hThread); CloseHandle(child.hProcess);
                    RecoveryLog(root, "已重新启动被控端，进程 " + std::to_string(child.dwProcessId)); return 0;
                }
                RecoveryLog(root, "恢复被控端失败，系统错误 " + std::to_string(GetLastError()));
            }
        }
        if (delay == INFINITE) { RecoveryLog(root, "无法保存恢复次数，已暂停自动重启"); return 0; }
        if (delay) {
            RecoveryLog(root, "十分钟内恢复次数达到上限，稍后重试");
            if (WaitForSingleObject(stop, delay) != WAIT_TIMEOUT) return 0;
        }
    }
    return 0;
}
