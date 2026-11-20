#pragma once

#include "../common.h"

// 返回可见的远程光标。收到隐藏状态或无可用图像时由观看窗口显示系统箭头。
HCURSOR DecodeRemoteCursor(const std::vector<uint8_t>& payload);
