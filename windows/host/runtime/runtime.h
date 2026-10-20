#pragma once
#include "../config.h"
#include "tray.h"
#include "keepalive.h"
#include "awake.h"
#include "idle_pulse.h"
#include "power_watch.h"
#include <functional>

class HostRuntime {
public:
    void Start(HWND window, const HostConfig& config, bool background, int show, const std::function<void()>& quit);
    void Stop();
    bool Handle(UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result);
    void Status(const std::wstring& status);
private:
    void Exit(const char* reason);
    void Show();
    bool Awake(bool enabled);
    void Save(const wchar_t* key, bool enabled);
    HWND window_ = nullptr;
    HostConfig config_;
    HostTray tray_;
    KeepAlive guard_;
    AwakeLease awake_;
    IdlePulse idle_;
    PowerWatch power_;
    std::wstring ini_, status_ = L"正在启动";
    std::function<void()> quit_;
    UINT taskbarCreated_ = 0;
    bool exiting_ = false;
    bool updateHandoff_ = false;
};
