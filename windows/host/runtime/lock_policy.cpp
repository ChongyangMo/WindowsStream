#include "lock_policy.h"
#include "session_lease.h"

namespace {
constexpr auto keyPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System";
constexpr auto valueName = L"DisableLockWorkstation";
constexpr auto section = L"lock_policy";
std::filesystem::path State(const std::filesystem::path& root) { return root / L".windowsstream-runtime" / L"awake-state.ini"; }
uint64_t Created(const std::filesystem::path& path) {
    wchar_t value[32]{}; GetPrivateProfileStringW(section, L"created", L"0", value, 32, path.c_str());
    return _wcstoui64(value, nullptr, 10);
}
void Notify() {
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"Policy"), SMTO_ABORTIFHUNG, 500, nullptr);
}
}

void SetTemporaryLockPolicy(const std::filesystem::path& root, uint64_t created) {
    SessionLeaseLock lease;
    if (!lease.locked) { LogLine("无法取得当前用户锁屏设置锁"); return; }
    HKEY key = nullptr;
    auto result = RegCreateKeyExW(HKEY_CURRENT_USER, keyPath, 0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr);
    if (result != ERROR_SUCCESS) { LogLine("当前用户禁止手动锁屏设置被拒绝：%ld，保留系统权限限制", result); return; }
    DWORD value = 0, type = 0, size = sizeof value;
    result = RegQueryValueExW(key, valueName, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size);
    if ((result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) || (result == ERROR_SUCCESS && (type != REG_DWORD || size != sizeof value))) {
        LogLine("读取当前用户锁屏设置失败：%ld，未修改", result); RegCloseKey(key); return;
    }
    const auto path = State(root);
    const bool pending = GetPrivateProfileIntW(section, L"pending", 0, path.c_str()) != 0;
    const bool originalExists = pending ? GetPrivateProfileIntW(section, L"original_exists", 0, path.c_str()) != 0 : result == ERROR_SUCCESS;
    const DWORD original = pending ? GetPrivateProfileIntW(section, L"original_value", 0, path.c_str()) : value;
    std::error_code error; std::filesystem::create_directories(path.parent_path(), error);
    const bool saved = !error && WritePrivateProfileStringW(section, L"original_exists", originalExists ? L"1" : L"0", path.c_str()) &&
        WritePrivateProfileStringW(section, L"original_value", std::to_wstring(original).c_str(), path.c_str()) &&
        WritePrivateProfileStringW(section, L"owner", std::to_wstring(GetCurrentProcessId()).c_str(), path.c_str()) &&
        WritePrivateProfileStringW(section, L"created", std::to_wstring(created).c_str(), path.c_str()) &&
        WritePrivateProfileStringW(section, L"pending", L"1", path.c_str());
    if (!saved) { LogLine("保存当前用户锁屏设置恢复记录失败，未修改策略"); RegCloseKey(key); return; }
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    const DWORD enabled = 1;
    result = RegSetValueExW(key, valueName, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&enabled), sizeof enabled);
    if (result != ERROR_SUCCESS) {
        WritePrivateProfileStringW(section, L"pending", L"0", path.c_str());
        LogLine("当前用户禁止手动锁屏设置被拒绝：%ld，保留系统权限限制", result);
    } else {
        size = sizeof value;
        const auto read = RegQueryValueExW(key, valueName, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size);
        if (read == ERROR_SUCCESS && type == REG_DWORD && value == 1) {
            LogLine("当前用户禁止锁定设置已写入并回读确认；退出时恢复原值"); Notify();
        } else LogLine("当前用户禁止锁定设置回读未确认，不能保证禁止手动锁屏");
    }
    RegCloseKey(key);
}

bool RestoreLockPolicy(const std::filesystem::path& root, DWORD owner, uint64_t created) {
    SessionLeaseLock lease;
    if (!lease.locked) return false;
    const auto path = State(root);
    if (!GetPrivateProfileIntW(section, L"pending", 0, path.c_str()) ||
        GetPrivateProfileIntW(section, L"owner", 0, path.c_str()) != owner || Created(path) != created) return true;
    HKEY key = nullptr;
    auto result = RegOpenKeyExW(HKEY_CURRENT_USER, keyPath, 0, KEY_SET_VALUE, &key);
    if (result == ERROR_SUCCESS) {
        if (GetPrivateProfileIntW(section, L"original_exists", 0, path.c_str())) {
            const DWORD value = GetPrivateProfileIntW(section, L"original_value", 0, path.c_str());
            result = RegSetValueExW(key, valueName, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof value);
        } else result = RegDeleteValueW(key, valueName);
        RegCloseKey(key);
    }
    if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) return false;
    WritePrivateProfileStringW(section, L"pending", L"0", path.c_str()); Notify(); return true;
}
void RestoreDormantLockPolicy(const std::filesystem::path& root) {
    const auto path = State(root);
    if (!GetPrivateProfileIntW(section, L"pending", 0, path.c_str())) return;
    if (!RestoreLockPolicy(root, GetPrivateProfileIntW(section, L"owner", 0, path.c_str()), Created(path)))
        LogLine("恢复上次当前用户锁屏设置失败，已保留恢复记录");
}
