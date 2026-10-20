#pragma once
#include "client.h"
#include <thread>

class IntranetPingWindow {
public:
    ~IntranetPingWindow() { Close(); }
    void Show(HWND owner, const std::wstring& ini);
    void Close();
private:
    static LRESULT CALLBACK Procedure(HWND, UINT, WPARAM, LPARAM);
    void Begin();
    void Poll();
    HWND window_ = nullptr, target_ = nullptr, button_ = nullptr, output_ = nullptr;
    std::wstring ini_;
    proxy::Tasks tasks_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::mutex mutex_;
    std::vector<std::wstring> pending_;
};
