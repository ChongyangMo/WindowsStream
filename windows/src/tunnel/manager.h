#pragma once
#include "../common.h"

class IpTunnel {
public:
    ~IpTunnel() { Stop(); }
    void Start(const std::wstring& ini);
    void Stop();
    bool Ready();
    bool Starting();
private:
    std::wstring Status();
    HANDLE process_ = nullptr, stop_ = nullptr;
    std::wstring state_, lastError_;
    bool attempted_ = false;
};
