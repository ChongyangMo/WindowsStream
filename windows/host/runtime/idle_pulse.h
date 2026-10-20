#pragma once
#include "../../src/common.h"

constexpr UINT_PTR kIdlePulseTimer = 31;

// 只发送零位移的鼠标活动，不包含按键、点击或滚动。
bool SubmitIdlePulse();

class IdlePulse {
public:
    void Start(HWND owner, bool enabled, unsigned seconds);
    void Stop();
    void Tick();
private:
    HWND owner_ = nullptr;
    unsigned seconds_ = 120;
    bool enabled_ = false, pending_ = false;
    DWORD inputBefore_ = 0;
    uint64_t lastAttempt_ = 0, lastSkipLog_ = 0;
};
