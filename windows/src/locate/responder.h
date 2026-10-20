#pragma once
#include "../common.h"
#include <thread>

// 反向模式下应答被控端的签名广播，被控端据此获得控制端当前地址。
class LocateResponder {
public:
    ~LocateResponder() { Stop(); }
    void Start(const std::wstring& bind, uint16_t port, const std::string& token);
    void Stop();
private:
    std::atomic<bool> running_{false};
    std::thread thread_;
};
