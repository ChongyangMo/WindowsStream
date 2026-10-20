#include "idle_pulse.h"

namespace {
constexpr ULONG_PTR marker = 0x57534950;  // 本程序空闲保活事件标记。
DWORD IdleMilliseconds(const LASTINPUTINFO& input) {
    const DWORD elapsed = GetTickCount() - input.dwTime;
    // 允许系统计数器回绕；时间戳略领先时不误判为数十天未操作。
    return elapsed < 0x80000000UL ? elapsed : 0;
}
bool KeysReleased() {
    for (int key = 1; key < 255; ++key) if (GetAsyncKeyState(key) & 0x8000) return false;
    return true;
}
bool DefaultDesktop() {
    HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!desktop) return false;
    wchar_t name[128]{}; DWORD bytes = 0;
    const bool ok = GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof name, &bytes) && _wcsicmp(name, L"Default") == 0;
    CloseDesktop(desktop);
    return ok;
}
}

bool SubmitIdlePulse() {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    input.mi.dx = input.mi.dy = 0;
    input.mi.dwExtraInfo = marker;
    return SendInput(1, &input, sizeof input) == 1;
}

void IdlePulse::Start(HWND owner, bool enabled, unsigned seconds) {
    Stop(); owner_ = owner; seconds_ = seconds;
    enabled_ = enabled && SetTimer(owner_, kIdlePulseTimer, 1000, nullptr) != 0;
    if (enabled_) LogLine("空闲保活已启用：连续 %u 秒无键鼠输入后发送零位移鼠标活动，不点击、不移动光标、不切换焦点", seconds_);
    else if (enabled) LogLine("启动空闲保活定时器失败：%lu", GetLastError());
}
void IdlePulse::Stop() {
    if (owner_) KillTimer(owner_, kIdlePulseTimer);
    owner_ = nullptr; enabled_ = pending_ = false; lastAttempt_ = lastSkipLog_ = 0;
}
void IdlePulse::Tick() {
    if (!enabled_) return;
    LASTINPUTINFO input{sizeof input};
    if (!GetLastInputInfo(&input)) return;
    const auto now = GetTickCount64();
    const DWORD idle = IdleMilliseconds(input);
    if (pending_) {
        if (input.dwTime != inputBefore_ && idle < 5000) LogLine("空闲保活已生效：系统空闲计时已刷新");
        else LogLine("空闲保活事件已提交，但系统空闲计时未确认刷新；不会回退为真实点击");
        pending_ = false;
    }
    const uint64_t interval = static_cast<uint64_t>(seconds_) * 1000;
    if (idle < interval || (lastAttempt_ && now - lastAttempt_ < interval)) return;
    if (!DefaultDesktop()) {
        if (!lastSkipLog_ || now - lastSkipLog_ >= 60000) {
            LogLine("当前为锁屏或安全桌面，跳过空闲保活，不尝试解锁"); lastSkipLog_ = now;
        }
        return;
    }
    if (!KeysReleased()) return;
    LASTINPUTINFO latest{sizeof latest};
    if (!GetLastInputInfo(&latest) || latest.dwTime != input.dwTime || IdleMilliseconds(latest) < interval) return;
    lastAttempt_ = now; inputBefore_ = latest.dwTime;
    SetLastError(ERROR_SUCCESS);
    if (SubmitIdlePulse()) {
        pending_ = true;
        LogLine("检测到键鼠空闲 %lu 秒，已发送一次零位移保活事件", idle / 1000);
    } else LogLine("系统拒绝空闲保活事件：%lu；可能受到高权限窗口或桌面权限限制", GetLastError());
}
