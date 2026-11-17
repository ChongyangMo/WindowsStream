#pragma once
#include "channel.h"
#include <functional>
#include <memory>

class HostServer {
public:
    void Run(const HostConfig& config, HWND owner, const std::function<void(const std::wstring&)>& status);
    void Stop();
    std::wstring ConnectedLocalAddress();
    unsigned Progress() const { return progress_.load(std::memory_order_relaxed); }
private:
    std::atomic<bool> running_{true};
    std::atomic<unsigned> progress_{0};
    std::mutex mutex_;
    std::shared_ptr<HostChannel> channel_;
    std::wstring connectedLocalAddress_;
};
