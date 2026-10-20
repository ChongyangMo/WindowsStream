#pragma once
#include "../config.h"
#include <thread>

class HostUpdateClient {
public:
    ~HostUpdateClient();
    void Start(const HostConfig& config, const std::string& bootId);
    void Stop();
private:
    void Check(const HostConfig& config, const std::string& bootId);
    std::atomic<bool> running_{false};
    std::thread thread_;
    uint64_t logOffset_ = 0;
};
