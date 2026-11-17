#include "autorun.h"
#include "../../shared/update_package.h"
#include <filesystem>
#include <string_view>

namespace {
constexpr auto kRelayLauncher = L"start-host.ps1";

bool RelayLauncherReady(const std::filesystem::path& root) {
    std::error_code error;
    return std::filesystem::is_regular_file(root / kRelayLauncher, error) &&
           std::filesystem::is_regular_file(root / L"WindowsStreamBridge.exe", error) &&
           std::filesystem::is_regular_file(root / L"relay.ini", error);
}
}

bool SetUserAutoStart(bool enabled, bool relay) {
    constexpr auto keyPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr auto name = L"WindowsStreamHost";
    const auto module = updates::ModulePath();
    const std::wstring argument = relay ? kRelayAutoStartArgument : L"--background";
    const auto command = L"\"" + module + L"\" " + argument;
    if (enabled && (module.empty() || command.size() > MAX_PATH ||
                    (relay && !RelayLauncherReady(std::filesystem::path(module).parent_path())))) {
        LogLine("无法注册登录自启，请检查程序路径和中转组件");
        return false;
    }
    HKEY key = nullptr;
    auto result = enabled ? RegCreateKeyExW(HKEY_CURRENT_USER, keyPath, 0, nullptr, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &key, nullptr)
                          : RegOpenKeyExW(HKEY_CURRENT_USER, keyPath, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, &key);
    if (!enabled && result == ERROR_FILE_NOT_FOUND) return true;
    if (result != ERROR_SUCCESS) { LogLine("打开当前用户启动项失败：%ld", result); return false; }
    wchar_t actual[MAX_PATH + 1]{};
    DWORD bytes = sizeof actual;
    const auto read = RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, actual, &bytes);
    if (enabled && (read != ERROR_SUCCESS || command != actual)) {
        result = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                               static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else if (!enabled && read == ERROR_SUCCESS && std::wstring_view(actual).ends_with(L" " + argument)) {
        result = RegDeleteValueW(key, name);
        if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    if (result != ERROR_SUCCESS) { LogLine("设置当前用户启动项失败：%ld", result); return false; }
    return true;
}

bool SaveUserAutoStart(bool enabled, bool relay, const std::wstring& ini) {
    wchar_t previous[32]{};
    GetPrivateProfileStringW(L"background", L"autostart", L"", previous, _countof(previous), ini.c_str());
    if (!WritePrivateProfileStringW(L"background", L"autostart", enabled ? L"1" : L"0", ini.c_str())) {
        LogLine("保存开机自启动设置失败：%lu", GetLastError());
        return false;
    }
    if (!SetUserAutoStart(enabled, relay)) {
        if (!WritePrivateProfileStringW(L"background", L"autostart", *previous ? previous : nullptr, ini.c_str()))
            LogLine("恢复开机自启动设置失败：%lu", GetLastError());
        return false;
    }
    LogLine("当前用户开机自启动：%s", enabled ? "已启用" : "已关闭");
    return true;
}

int LaunchRelayAutoStart() {
    const auto root = std::filesystem::path(PortableDir());
    if (root.empty() || !RelayLauncherReady(root)) {
        LogLine("中转自启失败，启动文件不完整");
        return 1;
    }
    wchar_t system[MAX_PATH]{};
    const auto length = GetSystemDirectoryW(system, MAX_PATH);
    if (!length || length >= MAX_PATH) {
        LogLine("无法读取系统目录，中转自启失败");
        return 1;
    }
    const auto powershell = std::filesystem::path(system) / L"WindowsPowerShell\\v1.0\\powershell.exe";
    auto command = L"\"" + powershell.wstring() + L"\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" +
                   (root / kRelayLauncher).wstring() + L"\" -Background";
    STARTUPINFOW startup{sizeof startup};
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(powershell.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, root.c_str(), &startup, &child)) {
        LogLine("无法启动中转脚本：%lu", GetLastError());
        return 1;
    }
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    return 0;
}
