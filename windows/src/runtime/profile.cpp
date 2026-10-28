#include "profile.h"

ControllerProfile LoadControllerProfile(const std::wstring& path) {
    ControllerProfile profile;
    wchar_t name[64]{};
    GetPrivateProfileStringW(L"connection", L"instance", L"", name, 64, path.c_str());
    const std::wstring instance = name;
    if (instance.empty()) return profile;
    if (instance.size() > 48 || instance.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::wstring::npos) {
        LogLine("独立窗口标识无效，使用默认窗口");
        return profile;
    }
    profile.mutex += L"-" + instance;
    profile.windowClass += L"-" + instance;
    wchar_t title[128]{};
    GetPrivateProfileStringW(L"connection", L"name", L"WindowsStream", title, 128, path.c_str());
    if (*title) profile.title = title;
    profile.clipboard = GetPrivateProfileIntW(L"permissions", L"clipboard", 1, path.c_str()) != 0;
    return profile;
}
