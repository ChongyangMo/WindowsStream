#include "system_settings.h"
#include <wininet.h>

namespace proxy {
SettingsLock::SettingsLock() {
    mutex = CreateMutexW(nullptr, FALSE, L"Local\\WindowsStreamProxySettings");
    if (mutex) { auto wait = WaitForSingleObject(mutex, 5000); locked = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED; }
}
SettingsLock::~SettingsLock() { if (locked) ReleaseMutex(mutex); if (mutex) CloseHandle(mutex); }
uint64_t Created(HANDLE process) {
    FILETIME created{}, exit{}, kernel{}, user{};
    return GetProcessTimes(process, &created, &exit, &kernel, &user) ?
        static_cast<uint64_t>(created.dwHighDateTime) << 32 | created.dwLowDateTime : 0;
}
bool ReadSystemSettings(SystemSettings& settings) {
    INTERNET_PER_CONN_OPTIONW options[4]{};
    options[0].dwOption = INTERNET_PER_CONN_FLAGS_UI;
    options[1].dwOption = INTERNET_PER_CONN_PROXY_SERVER;
    options[2].dwOption = INTERNET_PER_CONN_PROXY_BYPASS;
    options[3].dwOption = INTERNET_PER_CONN_AUTOCONFIG_URL;
    INTERNET_PER_CONN_OPTION_LISTW list{};
    list.dwSize = sizeof list; list.dwOptionCount = 4; list.pOptions = options;
    DWORD size = sizeof list;
    bool ok = InternetQueryOptionW(nullptr, INTERNET_OPTION_PER_CONNECTION_OPTION, &list, &size) != FALSE;
    if (ok) {
        settings.flags = options[0].Value.dwValue;
        settings.server = options[1].Value.pszValue ? options[1].Value.pszValue : L"";
        settings.bypass = options[2].Value.pszValue ? options[2].Value.pszValue : L"";
        settings.script = options[3].Value.pszValue ? options[3].Value.pszValue : L"";
    } else LogLine("读取系统代理设置失败：%lu", GetLastError());
    for (int i = 1; i < 4; ++i) if (options[i].Value.pszValue) GlobalFree(options[i].Value.pszValue);
    return ok;
}
bool WriteSystemSettings(const SystemSettings& settings) {
    INTERNET_PER_CONN_OPTIONW options[4]{};
    options[0].dwOption = INTERNET_PER_CONN_FLAGS; options[0].Value.dwValue = settings.flags;
    options[1].dwOption = INTERNET_PER_CONN_PROXY_SERVER; options[1].Value.pszValue = const_cast<wchar_t*>(settings.server.c_str());
    options[2].dwOption = INTERNET_PER_CONN_PROXY_BYPASS; options[2].Value.pszValue = const_cast<wchar_t*>(settings.bypass.c_str());
    options[3].dwOption = INTERNET_PER_CONN_AUTOCONFIG_URL; options[3].Value.pszValue = const_cast<wchar_t*>(settings.script.c_str());
    INTERNET_PER_CONN_OPTION_LISTW list{};
    list.dwSize = sizeof list; list.dwOptionCount = 4; list.pOptions = options;
    if (!InternetSetOptionW(nullptr, INTERNET_OPTION_PER_CONNECTION_OPTION, &list, sizeof list)) {
        LogLine("写入系统代理设置失败：%lu", GetLastError()); return false;
    }
    InternetSetOptionW(nullptr, INTERNET_OPTION_SETTINGS_CHANGED, nullptr, 0);
    InternetSetOptionW(nullptr, INTERNET_OPTION_REFRESH, nullptr, 0);
    SystemSettings actual;
    return ReadSystemSettings(actual) && actual == settings;
}
namespace {
std::wstring Hex(const std::wstring& text) {
    constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring hex;
    for (wchar_t c : text) for (int shift = 12; shift >= 0; shift -= 4) hex += digits[(c >> shift) & 15];
    return hex;
}
bool Decode(const std::wstring& hex, std::wstring& text) {
    text.clear();
    if (hex.size() % 4) return false;
    for (size_t i = 0; i < hex.size(); i += 4) {
        unsigned value = 0;
        for (size_t j = i; j < i + 4; ++j) {
            const auto digit = std::wstring(L"0123456789abcdef").find(hex[j]);
            if (digit == std::wstring::npos) return false;
            value = value * 16 + static_cast<unsigned>(digit);
        }
        if (!value) return false;
        text += static_cast<wchar_t>(value);
    }
    return true;
}
bool Store(const std::wstring& path, const wchar_t* section, const SystemSettings& settings) {
    if (settings.server.size() > 8000 || settings.bypass.size() > 8000 || settings.script.size() > 8000) return false;
    return WritePrivateProfileStringW(section, L"flags", std::to_wstring(settings.flags).c_str(), path.c_str()) &&
        WritePrivateProfileStringW(section, L"server", Hex(settings.server).c_str(), path.c_str()) &&
        WritePrivateProfileStringW(section, L"bypass", Hex(settings.bypass).c_str(), path.c_str()) &&
        WritePrivateProfileStringW(section, L"script", Hex(settings.script).c_str(), path.c_str());
}
bool Load(const std::wstring& path, const wchar_t* section, SystemSettings& settings) {
    settings.flags = GetPrivateProfileIntW(section, L"flags", 0xFFFFFFFF, path.c_str());
    if (settings.flags == 0xFFFFFFFF) return false;
    auto read = [&](const wchar_t* key, std::wstring& value) {
        wchar_t buffer[32768]{};
        const DWORD count = GetPrivateProfileStringW(section, key, L"", buffer, 32768, path.c_str());
        return count < 32767 && Decode(buffer, value);
    };
    return read(L"server", settings.server) && read(L"bypass", settings.bypass) && read(L"script", settings.script);
}
}
bool SaveSnapshot(const std::wstring& path, const ProxySnapshot& state) {
    const auto temporary = path + L".tmp";
    DeleteFileW(temporary.c_str());
    const bool written = WritePrivateProfileStringW(L"owner", L"process", std::to_wstring(state.process).c_str(), temporary.c_str()) &&
        WritePrivateProfileStringW(L"owner", L"created", std::to_wstring(state.created).c_str(), temporary.c_str()) &&
        Store(temporary, L"original", state.original) && Store(temporary, L"applied", state.applied);
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, temporary.c_str());
    ProxySnapshot verified;
    const bool saved = written && LoadSnapshot(temporary, verified) && verified.process == state.process && verified.created == state.created &&
        verified.original == state.original && verified.applied == state.applied &&
        MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!saved) { LogLine("保存系统代理恢复记录失败：%lu", GetLastError()); DeleteFileW(temporary.c_str()); }
    return saved;
}
bool LoadSnapshot(const std::wstring& path, ProxySnapshot& state) {
    state.process = GetPrivateProfileIntW(L"owner", L"process", 0, path.c_str());
    wchar_t value[32]{}; GetPrivateProfileStringW(L"owner", L"created", L"0", value, 32, path.c_str());
    state.created = _wcstoui64(value, nullptr, 10);
    return state.process && state.created && Load(path, L"original", state.original) && Load(path, L"applied", state.applied);
}
bool RestoreSnapshot(const std::wstring& path, DWORD owner, uint64_t created) {
    SettingsLock lock;
    if (!lock.locked) return false;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return true;
    ProxySnapshot state;
    if (!LoadSnapshot(path, state)) { LogLine("系统代理恢复记录无法读取，保留原文件"); return false; }
    if (owner && (owner != state.process || created != state.created)) return true;
    if (!owner) {
        HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, state.process);
        if (process) {
            const bool alive = Created(process) == state.created && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
            CloseHandle(process); if (alive) return false;
        }
    }
    SystemSettings current;
    if (!ReadSystemSettings(current)) return false;
    if (current == state.applied) {
        if (!WriteSystemSettings(state.original)) { LogLine("恢复原系统代理失败，保留恢复记录"); return false; }
        LogLine("原系统代理设置已恢复");
    } else if (!(current == state.original)) LogLine("系统代理已被其他程序更改，保留当前设置");
    return DeleteFileW(path.c_str()) != FALSE;
}
}
