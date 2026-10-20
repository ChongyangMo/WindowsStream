#include "autorun.h"
#include "../../shared/update_package.h"

bool SetUserAutoStart(bool enabled) {
    constexpr auto keyPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr auto name = L"WindowsStreamHost";
    HKEY key = nullptr;
    auto result = enabled ? RegCreateKeyExW(HKEY_CURRENT_USER, keyPath, 0, nullptr, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &key, nullptr)
                          : RegOpenKeyExW(HKEY_CURRENT_USER, keyPath, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, &key);
    if (!enabled && result == ERROR_FILE_NOT_FOUND) return true;
    if (result != ERROR_SUCCESS) { LogLine("打开当前用户启动项失败：%ld", result); return false; }
    if (enabled) {
        const auto command = L"\"" + updates::ModulePath() + L"\" --background";
        result = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                               static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        if (result == ERROR_SUCCESS) {
            wchar_t actual[32768]{};
            DWORD bytes = sizeof actual, type = 0;
            result = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(actual), &bytes);
            if (result == ERROR_SUCCESS && (type != REG_SZ || command != actual)) result = ERROR_INVALID_DATA;
        }
    } else {
        result = RegDeleteValueW(key, name);
        if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    if (result != ERROR_SUCCESS) { LogLine("设置当前用户启动项失败：%ld", result); return false; }
    LogLine("当前用户登录后自动启动：%s，已回读确认", enabled ? "已启用" : "已关闭");
    return true;
}
