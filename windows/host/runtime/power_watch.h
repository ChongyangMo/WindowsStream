#pragma once
#include "../../src/common.h"

constexpr UINT_PTR kPowerWatchTimer = 32;

// 记录交流电/电池切换和有线链路通断；断线期间的记录在重连后随诊断日志补传到控制端。
class PowerWatch {
public:
    void Start(HWND window);
    void Stop();
    void Tick();
private:
    HWND window_ = nullptr;
    int ac_ = -1;
    int64_t link_ = -1;
};
