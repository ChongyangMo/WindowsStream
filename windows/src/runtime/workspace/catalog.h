#pragma once
#include "../../common.h"

namespace workspace {
struct Profile { std::wstring name, path; bool primary = false; };
std::wstring ExecutablePath();
std::wstring DefaultConfig();
std::wstring CanonicalPath(const std::wstring& path);
std::wstring ReadSetting(const std::wstring& path, const wchar_t* section, const wchar_t* key, const wchar_t* fallback = L"");
std::vector<Profile> LoadProfiles(const std::wstring& base);
bool SaveProfile(const std::wstring& base, const std::wstring& host, unsigned port,
                 const std::wstring& token, Profile& profile, std::wstring& error);
}
