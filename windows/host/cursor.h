#pragma once
#include "channel.h"

// 指针形状单独发送，控制端即时显示系统指针，不再等待桌面视频返回。
class CursorSender {
public:
    bool Poll(HostChannel& channel);
private:
    HCURSOR previous_ = nullptr;
    bool visible_ = false, initialized_ = false;
};
