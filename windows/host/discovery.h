#pragma once
#include "config.h"
#include <thread>

class DiscoveryResponder {
public:
    ~DiscoveryResponder();
    void Start(const HostConfig& config);
private:
    std::atomic<bool> running_{false};
    std::thread thread_;
};
