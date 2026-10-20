#pragma once
#include "../common.h"
#include <shellapi.h>

constexpr UINT kControllerTrayCallback = WM_APP + 20;
constexpr UINT kControllerTrayShow = 42001, kControllerTrayExit = 42002;

class ControllerTray {
public:
    ~ControllerTray() { Stop(); }
    void Start(HWND window);
    void Stop();
    bool Handle(UINT message, WPARAM wParam, LPARAM lParam);
private:
    bool Add();
    void Show();
    void Menu();
    HWND window_ = nullptr;
    NOTIFYICONDATAW icon_{};
    UINT taskbarCreated_ = 0;
    ULONGLONG retry_ = 0;
    bool added_ = false;
};
