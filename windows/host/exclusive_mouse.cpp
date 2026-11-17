#include "exclusive_mouse.h"
#include "service/service.h"

namespace {
constexpr int kReleaseHotkey = 1;
constexpr ULONGLONG kTimeout = 60'000;
thread_local ULONGLONG endAt = 0;

LRESULT CALLBACK FilterMouse(int code, WPARAM message, LPARAM data) {
    if (code == HC_ACTION && GetTickCount64() < endAt) {
        const auto& mouse = *reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
        if (!(mouse.flags & LLMHF_INJECTED) || mouse.dwExtraInfo != kWindowsStreamMouseInput) return 1;
    }
    return CallNextHookEx(nullptr, code, message, data);
}

bool MouseButtonHeld() {
    for (const int key : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2}) {
        if (GetAsyncKeyState(key) & 0x8000) return true;
    }
    return false;
}
}

ExclusiveMouse::~ExclusiveMouse() { Stop(); }

DWORD ExclusiveMouse::Start(HWND owner) {
    if (Active()) return ERROR_SUCCESS;
    Stop();
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_) return GetLastError();
    try {
        std::promise<DWORD> ready;
        auto result = ready.get_future();
        worker_ = std::thread(&ExclusiveMouse::Run, this, owner, std::move(ready));
        return result.get();
    } catch (...) {
        Stop();
        return ERROR_NOT_ENOUGH_MEMORY;
    }
}

void ExclusiveMouse::Stop() {
    if (!stop_) return;
    SetEvent(stop_);
    if (worker_.joinable()) worker_.join();
    CloseHandle(stop_);
    stop_ = nullptr;
}

void ExclusiveMouse::Run(HWND owner, std::promise<DWORD> ready) {
    if (!servermode::DesktopAccessible()) { ready.set_value(ERROR_ACCESS_DENIED); return; }
    if (MouseButtonHeld()) { ready.set_value(ERROR_BUSY); return; }
    if (!RegisterHotKey(nullptr, kReleaseHotkey, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F11)) {
        ready.set_value(GetLastError());
        return;
    }
    const auto hook = SetWindowsHookExW(WH_MOUSE_LL, FilterMouse, GetModuleHandleW(nullptr), 0);
    if (!hook) {
        ready.set_value(GetLastError());
        UnregisterHotKey(nullptr, kReleaseHotkey);
        return;
    }
    endAt = GetTickCount64() + kTimeout;
    active_ = true;
    PostMessageW(owner, kHostExclusiveMouseStatus, TRUE, 0);
    ready.set_value(ERROR_SUCCESS);
    bool release = false;
    while (!release && GetTickCount64() < endAt && servermode::DesktopAccessible()) {
        const auto wait = MsgWaitForMultipleObjects(1, &stop_, FALSE, 200, QS_ALLINPUT);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED) break;
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_HOTKEY || message.message == WM_QUIT) { release = true; break; }
        }
    }
    endAt = 0;
    UnhookWindowsHookEx(hook);
    UnregisterHotKey(nullptr, kReleaseHotkey);
    active_ = false;
    PostMessageW(owner, kHostExclusiveMouseStatus, FALSE, 0);
}
