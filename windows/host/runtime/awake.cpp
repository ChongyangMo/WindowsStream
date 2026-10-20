#include "awake.h"
#include "keepalive.h"
#include "session_lease.h"
#include "lock_policy.h"

namespace {
constexpr auto section = L"screen_saver";
constexpr EXECUTION_STATE requirements = ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED;
std::filesystem::path StatePath(const std::filesystem::path& root) { return root / L".windowsstream-runtime" / L"awake-state.ini"; }
uint64_t ReadCreated(const std::filesystem::path& path) {
    wchar_t text[32]{};
    GetPrivateProfileStringW(section, L"created", L"0", text, 32, path.c_str());
    return _wcstoui64(text, nullptr, 10);
}
bool ReadScreenSaver(BOOL& active) { return SystemParametersInfoW(SPI_GETSCREENSAVEACTIVE, 0, &active, 0) != FALSE; }
bool SetScreenSaver(BOOL active) {
    // 只改当前登录会话，不使用 SPIF_UPDATEINIFILE，不永久改用户配置。
    if (!SystemParametersInfoW(SPI_SETSCREENSAVEACTIVE, active ? TRUE : FALSE, nullptr, SPIF_SENDCHANGE)) return false;
    BOOL actual = FALSE;
    return ReadScreenSaver(actual) && !!actual == !!active;
}
}

bool RestoreScreenSaver(const std::filesystem::path& root, DWORD owner, uint64_t created) {
    SessionLeaseLock lease;
    if (!lease.locked) return false;
    const auto path = StatePath(root);
    if (!GetPrivateProfileIntW(section, L"pending", 0, path.c_str()) ||
        GetPrivateProfileIntW(section, L"owner", 0, path.c_str()) != owner || ReadCreated(path) != created) return true;
    const BOOL original = GetPrivateProfileIntW(section, L"original_active", 0, path.c_str()) != 0;
    if (!SetScreenSaver(original)) return false;
    return WritePrivateProfileStringW(section, L"pending", L"0", path.c_str()) != FALSE;
}

void RestoreDormantScreenSaver(const std::filesystem::path& root) {
    RestoreDormantLockPolicy(root);
    const auto path = StatePath(root);
    if (!GetPrivateProfileIntW(section, L"pending", 0, path.c_str())) return;
    if (RestoreScreenSaver(root, GetPrivateProfileIntW(section, L"owner", 0, path.c_str()), ReadCreated(path)))
        LogLine("已恢复上次遗留的当前会话屏保状态");
    else LogLine("恢复上次屏保状态失败，已保留恢复记录");
}

void AwakeLease::DisableScreenSaver() {
    SessionLeaseLock lease;
    if (!lease.locked) { LogLine("无法取得屏保恢复记录锁，未更改屏保"); return; }
    BOOL active = FALSE;
    if (!ReadScreenSaver(active)) { LogLine("读取当前会话屏保状态失败：%lu", GetLastError()); return; }
    const auto path = StatePath(root_);
    const bool pending = GetPrivateProfileIntW(section, L"pending", 0, path.c_str()) != 0;
    if (!active && !pending) { LogLine("当前会话屏保已关闭"); return; }
    const BOOL original = pending ? GetPrivateProfileIntW(section, L"original_active", 0, path.c_str()) != 0 : active;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) { LogLine("保存屏保恢复记录失败：%d", error.value()); return; }
    const bool saved = WritePrivateProfileStringW(section, L"original_active", original ? L"1" : L"0", path.c_str()) &&
        WritePrivateProfileStringW(section, L"owner", std::to_wstring(GetCurrentProcessId()).c_str(), path.c_str()) &&
        WritePrivateProfileStringW(section, L"created", std::to_wstring(created_).c_str(), path.c_str()) &&
        WritePrivateProfileStringW(section, L"pending", L"1", path.c_str());
    if (!saved) { LogLine("保存屏保恢复记录失败：%lu", GetLastError()); return; }
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    if (SetScreenSaver(FALSE)) LogLine("当前会话屏保已暂停，空闲屏保锁屏已抑制；退出时恢复原状态");
    else LogLine("暂停当前会话屏保失败或被策略拒绝：%lu", GetLastError());
}

void AwakeLease::LogPolicy() {
    DWORD seconds = 0, size = sizeof seconds;
    const auto result = RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
        L"InactivityTimeoutSecs", RRF_RT_REG_DWORD, nullptr, &seconds, &size);
    if (result == ERROR_SUCCESS && seconds) LogLine("检测到系统强制空闲锁屏策略：%lu 秒，普通权限无法保证阻止", seconds);
    else if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) LogLine("读取系统空闲锁屏策略失败：%ld", result);
    LogLine("常亮保护只作用于当前会话；手动锁屏、强制安全策略和主动睡眠仍由 Windows 决定");
}

bool AwakeLease::Enable() {
    const auto previous = SetThreadExecutionState(requirements);
    if (!previous) { LogLine("启用系统和屏幕常亮失败：%lu", GetLastError()); return false; }
    root_ = PortableDir(); created_ = ProcessCreation(GetCurrentProcess()); enabled_ = true;
    const auto verified = SetThreadExecutionState(requirements);
    LogLine("常亮保护已启用：禁止自动息屏、禁止自动睡眠；系统与显示请求回读 %s",
        (verified & (ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED)) == (ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED) ? "通过" : "未确认");
    DisableScreenSaver(); SetTemporaryLockPolicy(root_, created_); LogPolicy();
    return true;
}
void AwakeLease::Refresh() {
    if (!enabled_) return;
    if (!SetThreadExecutionState(requirements)) LogLine("续期常亮保护失败：%lu", GetLastError());
    BOOL active = FALSE;
    if (ReadScreenSaver(active) && active) DisableScreenSaver();
}
void AwakeLease::Release(bool preserveScreenSaver) {
    SetThreadExecutionState(ES_CONTINUOUS);
    if (!enabled_) return;
    enabled_ = false;
    if (preserveScreenSaver) { LogLine("升级期间保留屏保抑制，由新进程接管恢复记录"); return; }
    if (RestoreScreenSaver(root_, GetCurrentProcessId(), created_)) LogLine("常亮保护已解除，当前会话屏保已恢复原状态");
    else LogLine("恢复当前会话屏保失败：%lu，已保留恢复记录", GetLastError());
    if (!RestoreLockPolicy(root_, GetCurrentProcessId(), created_)) LogLine("恢复当前用户锁屏设置失败，已保留恢复记录");
}
