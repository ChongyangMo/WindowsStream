#pragma once
#include "config.h"

// 连接建立方向与控制方向独立：无管理员权限的被控端可以主动连接控制端。
class HostChannel {
public:
    ~HostChannel();
    // controller 为反向模式当前要连接的控制端地址。
    bool Open(const HostConfig& config, const std::wstring& controller, std::atomic<bool>& running);
    bool Authenticate(const std::string& token, int width, int height, int fps);
    bool Read(uint8_t& type, std::vector<uint8_t>& payload);
    bool Send(uint8_t type, const Writer& payload);
    void Shutdown();
    static void CloseListener();
private:
    bool ReadExact(void* data, size_t size);
    SOCKET socket_ = INVALID_SOCKET;
    static SOCKET listener_;
    std::mutex sendMutex_;
};
