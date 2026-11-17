#pragma once
#include "../src/common.h"
#include <future>
#include <thread>

constexpr UINT kHostExclusiveMouseStatus = WM_APP + 34;
constexpr ULONG_PTR kWindowsStreamMouseInput = 0x57534D32;

class ExclusiveMouse {
public:
    ~ExclusiveMouse();
    DWORD Start(HWND owner);
    void Stop();
    bool Active() const { return active_.load(); }

private:
    void Run(HWND owner, std::promise<DWORD> ready);
    HANDLE stop_ = nullptr;
    std::thread worker_;
    std::atomic<bool> active_{false};
};
