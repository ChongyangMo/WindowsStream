#pragma once
#include "../../src/common.h"
#include <shellapi.h>

constexpr UINT kTrayCallback = WM_APP + 20;
constexpr UINT kTrayShow = 41001, kTrayAutoStart = 41003, kTrayKeepAwake = 41004;

class HostTray {
public:
    bool Add(HWND owner);
    void Remove();
    void Status(const std::wstring& status);
    void Menu(bool autoStart, bool keepAwake);
    bool Added() const { return added_; }
private:
    NOTIFYICONDATAW data_{};
    bool added_ = false;
};
