#pragma once
#include "channel.h"
#include <functional>
#include <memory>

class HostServer {
public:
    void Run(const HostConfig& config, HWND owner, const std::function<void(const std::wstring&)>& status);
    void Stop();
private:
    std::atomic<bool> running_{true};
    std::mutex mutex_;
    std::shared_ptr<HostChannel> channel_;
};
