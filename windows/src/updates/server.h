#pragma once
#include "../common.h"
#include "../../shared/update_package.h"
#include <thread>

class UpdateServer {
public:
    ~UpdateServer();
    void Start(const std::wstring& bind, uint16_t port, const std::string& token, const std::wstring& package);
    void StartConfigured(const std::wstring& config, const std::wstring& bind, const std::string& token);
    void Stop();
private:
    void Handle(SOCKET peer);
    SOCKET Listen(const std::wstring& bind, uint16_t port, std::string& lastError);
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::string token_;
    std::wstring packagePath_;
    updates::Package package_;
};
