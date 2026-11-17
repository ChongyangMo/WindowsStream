#pragma once
#include "../src/common.h"

class InputDiagnostics {
public:
    // 返回可写入控制端日志的一条回执，普通点击回执限频。
    std::string MouseButton(const std::vector<uint8_t>& payload, bool submitted, bool exclusive);
private:
    uint64_t nextReport_ = 0;
    HWND previousForeground_ = nullptr, previousTarget_ = nullptr;
};
