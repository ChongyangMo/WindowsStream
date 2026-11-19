#pragma once
#include "pipe.h"
#include "../net.h"
#include <condition_variable>
#include <memory>
#include <thread>

namespace remotefiles {
class ViewerFiles {
public:
    void Start(const std::wstring& profile);
    void Stop();
    void Connect(const std::shared_ptr<Connection>& connection);
    void Disconnect();
    bool Receive(uint8_t type, std::vector<uint8_t>& payload);
private:
    void Serve(const std::wstring& name);
    Writer Request(const uint8_t* data, DWORD size);
    std::mutex mutex_;
    std::condition_variable changed_;
    std::shared_ptr<Connection> connection_;
    bool available_ = false;
    uint64_t next_ = 0, pending_ = 0;
    std::vector<uint8_t> response_;
    filedrag::FileHandle stop_;
    std::thread worker_;
};

int DispatchCommand();
}
