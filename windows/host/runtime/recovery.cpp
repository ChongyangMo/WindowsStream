#include "keepalive.h"
#include "awake.h"
#include "lock_policy.h"
#include "../../shared/update_package.h"
#include <fstream>

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
bool Budget(const std::filesystem::path& root) {
    const auto path = root / L".windowsstream-runtime" / L"recovery-state.ini";
    wchar_t value[32]{};
    GetPrivateProfileStringW(L"recovery", L"window_start", L"0", value, 32, path.c_str());
    const uint64_t start = _wcstoui64(value, nullptr, 10), now = GetTickCount64();
    auto count = GetPrivateProfileIntW(L"recovery", L"attempts", 0, path.c_str());
    if (!start || now < start || now - start >= 60000) {
        count = 0;
        WritePrivateProfileStringW(L"recovery", L"window_start", std::to_wstring(now).c_str(), path.c_str());
    }
    if (count >= 5) return false;
    WritePrivateProfileStringW(L"recovery", L"attempts", std::to_wstring(count + 1).c_str(), path.c_str());
    return true;
}
}

int RunRecovery(DWORD parentId, uint64_t created, HANDLE parent, HANDLE stop) {
    struct Handles { HANDLE parent, stop; ~Handles() { if (parent) CloseHandle(parent); if (stop) CloseHandle(stop); } } handles{parent, stop};
    const auto module = std::filesystem::path(updates::ModulePath());
    const auto hash = WideToUtf8(module.parent_path().filename().wstring());
    const auto root = module.parent_path().parent_path().parent_path();
    if (!parent || !stop || module.filename() != L"WindowsStreamKeepAlive.exe" ||
        module.parent_path().parent_path().filename() != L".windowsstream-runtime" || hash.size() != 64 ||
        hash.find_first_not_of("0123456789abcdef") != std::string::npos || GetProcessId(parent) != parentId ||
        !created || ProcessCreation(parent) != created) return 2;
    std::wstring path(32768, L'\0'); DWORD length = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(parent, 0, path.data(), &length)) return 2;
    path.resize(length);
    std::error_code error;
    const auto target = root / L"WindowsStreamHost.exe";
    if (!std::filesystem::equivalent(target, path, error) || error) return 2;
    HANDLE waited[] = {stop, parent};
    const auto result = WaitForMultipleObjects(2, waited, FALSE, INFINITE);
    if (result != WAIT_OBJECT_0 + 1) return 0;
    DWORD code = 0;
    if (!GetExitCodeProcess(parent, &code) || code == 0) {
        if (!RestoreScreenSaver(root, parentId, created)) RecoveryLog(root, "恢复原屏保状态失败，保留恢复记录");
        if (!RestoreLockPolicy(root, parentId, created)) RecoveryLog(root, "恢复原锁屏设置失败，保留恢复记录");
        return 0;
    }
    RecoveryLog(root, "检测到被控端异常退出，退出码 " + std::to_string(code) + "，准备恢复");
    for (;;) {
        if (WaitForSingleObject(stop, 2000) != WAIT_TIMEOUT ||
            !GetPrivateProfileIntW(L"background", L"watchdog", 1, (root / L"host.ini").c_str())) {
            if (!RestoreScreenSaver(root, parentId, created)) RecoveryLog(root, "恢复原屏保状态失败，保留恢复记录");
            if (!RestoreLockPolicy(root, parentId, created)) RecoveryLog(root, "恢复原锁屏设置失败，保留恢复记录");
            return 0;
        }
        HANDLE active = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\WindowsStreamHost");
        if (active) { CloseHandle(active); RestoreScreenSaver(root, parentId, created); RestoreLockPolicy(root, parentId, created); return 0; }
        if (!Budget(root)) {
            RecoveryLog(root, "一分钟内恢复次数达到上限，等待一分钟后重试");
            if (WaitForSingleObject(stop, 60000) != WAIT_TIMEOUT) return 0;
            continue;
        }
        std::wstring command = L"\"" + target.wstring() + L"\" --background";
        STARTUPINFOW startup{sizeof startup};
        startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION child{};
        if (CreateProcessW(target.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                           root.c_str(), &startup, &child)) {
            CloseHandle(child.hThread); CloseHandle(child.hProcess);
            RecoveryLog(root, "被控端已恢复，进程 " + std::to_string(child.dwProcessId)); return 0;
        }
        RecoveryLog(root, "恢复被控端失败，系统错误 " + std::to_string(GetLastError()));
    }
}
