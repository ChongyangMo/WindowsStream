#include "catalog.h"
#include <filesystem>
#include <shlobj.h>
#include <cwctype>

namespace workspace {
std::wstring ExecutablePath() {
    wchar_t path[32768]{};
    const auto length = GetModuleFileNameW(nullptr, path, 32768);
    return length && length < 32768 ? std::wstring(path, length) : L"";
}
std::wstring CanonicalPath(const std::wstring& path) {
    std::error_code error;
    auto full = std::filesystem::absolute(path, error);
    std::wstring result = error ? path : full.lexically_normal().wstring();
    std::transform(result.begin(), result.end(), result.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return result;
}
std::wstring DefaultConfig() {
    const auto portable = std::filesystem::path(ExecutablePath()).parent_path() / L"client.ini";
    if (GetFileAttributesW(portable.c_str()) != INVALID_FILE_ATTRIBUTES) return portable.wstring();
    PWSTR base = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &base)))
        result = (std::filesystem::path(base) / L"WindowsStream" / L"client.ini").wstring();
    CoTaskMemFree(base);
    return result;
}
std::wstring ReadSetting(const std::wstring& path, const wchar_t* section, const wchar_t* key, const wchar_t* fallback) {
    wchar_t text[32768]{};
    GetPrivateProfileStringW(section, key, fallback, text, 32768, path.c_str());
    return text;
}
std::vector<Profile> LoadProfiles(const std::wstring& base) {
    std::vector<Profile> profiles;
    const bool primary = GetPrivateProfileIntW(L"proxy", L"enabled", 1, base.c_str()) != 0 ||
        GetPrivateProfileIntW(L"updates", L"enabled", 1, base.c_str()) != 0;
    profiles.push_back({ReadSetting(base, L"connection", L"name", L"主连接"), base, primary});
    const auto catalog = std::filesystem::path(base).parent_path() / L"workspace.ini";
    for (unsigned i = 0; i < 64; ++i) {
        const auto section = L"connection." + std::to_wstring(i);
        const auto path = ReadSetting(catalog.wstring(), section.c_str(), L"path");
        if (path.empty()) continue;
        const auto full = std::filesystem::path(path).is_absolute() ? std::filesystem::path(path) : catalog.parent_path() / path;
        if (GetFileAttributesW(full.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        profiles.push_back({ReadSetting(full.wstring(), L"connection", L"name", L"WindowsStream"), full.wstring(), false});
    }
    std::error_code error;
    const auto folder = std::filesystem::path(base).parent_path() / L"connections";
    for (std::filesystem::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
        const auto path = it->path() / L"client.ini";
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
            profiles.push_back({ReadSetting(path.wstring(), L"connection", L"name", L"WindowsStream"), path.wstring(), false});
    }
    return profiles;
}
bool SaveProfile(const std::wstring& base, const std::wstring& host, unsigned port,
                 const std::wstring& token, Profile& profile, std::wstring& error) {
    if (host.empty() || host.size() > 253 || host.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_") != std::wstring::npos) {
        error = L"请输入电脑地址"; return false;
    }
    if (!port || port > 65535) { error = L"端口范围为 1 到 65535"; return false; }
    if (WideToUtf8(token).size() < 32 || token.find_first_of(L"\r\n") != std::wstring::npos) {
        error = L"连接口令至少需要 32 个字节"; return false;
    }
    // 相同目标复用配置，避免重复打开时抢占同一被控端。
    for (const auto& saved : LoadProfiles(base)) {
        if (ReadSetting(saved.path, L"connection", L"host") == host &&
            GetPrivateProfileIntW(L"connection", L"port", 7400, saved.path.c_str()) == port &&
            ReadSetting(saved.path, L"connection", L"token") == token) { profile = saved; return true; }
    }
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) { error = L"无法创建连接配置"; return false; }
    wchar_t id[40]{}; StringFromGUID2(guid, id, 40);
    std::wstring key = id; key.erase(std::remove_if(key.begin(), key.end(), [](wchar_t c) { return c == L'{' || c == L'}'; }), key.end());
    const auto folder = std::filesystem::path(base).parent_path() / L"connections" / key;
    std::error_code code; std::filesystem::create_directories(folder, code);
    if (code) { error = L"无法保存连接配置"; return false; }
    profile = {host + L":" + std::to_wstring(port), (folder / L"client.ini").wstring(), false};
    HANDLE file = CreateFileW(profile.path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { error = L"无法保存连接配置"; return false; }
    const WORD bom = 0xFEFF; DWORD written = 0;
    const bool created = WriteFile(file, &bom, sizeof bom, &written, nullptr) && written == sizeof bom;
    CloseHandle(file);
    const auto write = [&](const wchar_t* section, const wchar_t* name, const std::wstring& value) {
        return WritePrivateProfileStringW(section, name, value.c_str(), profile.path.c_str()) != FALSE;
    };
    if (!created || !write(L"connection", L"instance", key) || !write(L"connection", L"name", profile.name) ||
        !write(L"connection", L"host", host) || !write(L"connection", L"port", std::to_wstring(port)) ||
        !write(L"connection", L"bind_host", L"0.0.0.0") || !write(L"connection", L"token", token) ||
        !write(L"updates", L"enabled", L"0") || !write(L"proxy", L"enabled", L"0") ||
        !write(L"proxy", L"system_proxy", L"0")) {
        error = L"保存连接配置失败"; DeleteFileW(profile.path.c_str()); return false;
    }
    LogLine("已保存独立远程连接：%s", WideToUtf8(profile.name).c_str());
    return true;
}
}
