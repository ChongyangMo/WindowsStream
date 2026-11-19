#pragma once
#include "../resources/resource.h"
#include "../src/common.h"

inline const std::wstring& HostVersion() {
    static const std::wstring version = [] {
        wchar_t text[32]{};
        const auto length = LoadStringW(GetModuleHandleW(nullptr), IDS_HOST_VERSION, text, ARRAYSIZE(text));
        return length ? std::wstring(text, length) : std::wstring(L"未知");
    }();
    return version;
}
