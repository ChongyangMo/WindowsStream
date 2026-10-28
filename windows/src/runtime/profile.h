#pragma once
#include "../common.h"

struct ControllerProfile {
    std::wstring mutex = L"Local\\WindowsStreamClient";
    std::wstring windowClass = L"WindowsStreamWindow";
    std::wstring title = L"WindowsStream";
    bool clipboard = true;
};

ControllerProfile LoadControllerProfile(const std::wstring& path, bool independent = false);
