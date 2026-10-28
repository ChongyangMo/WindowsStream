#pragma once
#include "../../src/common.h"
#include <functional>
#include <memory>
#include <thread>

namespace proxy {
struct Socket {
    explicit Socket(SOCKET value) : value(value) {}
    ~Socket() { if (value != INVALID_SOCKET) closesocket(value); }
    Socket(const Socket&) = delete;
    SOCKET value;
};
using Peer = std::shared_ptr<Socket>;

// 统一管理线程和套接字；退出时先 shutdown，再等待任务结束，避免句柄复用竞态。
class Tasks {
public:
    ~Tasks() { Stop(); }
    void Start() { running = true; }
    void Stop();
    Peer Track(SOCKET socket);
    // 借用其他任务组的套接字；停止时一并 shutdown，不重复拥有或关闭句柄。
    bool Watch(const Peer& peer);
    bool Run(std::function<void()> work);
    std::atomic<bool> running{false};
private:
    struct Task { std::thread thread; std::shared_ptr<std::atomic<bool>> done; };
    std::mutex mutex_;
    std::vector<std::weak_ptr<Socket>> sockets_;
    std::vector<Task> tasks_;
};
void Timeout(SOCKET socket, DWORD milliseconds);
bool Read(SOCKET socket, void* data, size_t size);
bool Send(SOCKET socket, const void* data, size_t size);
bool Text(SOCKET socket, const std::string& text);
bool Ready(SOCKET socket, bool write, unsigned milliseconds);
Peer Listen(Tasks& tasks, const std::wstring& address, uint16_t port, int* error = nullptr);
Peer Connect(Tasks& tasks, const std::wstring& host, uint16_t port, uint8_t& error);
void Relay(Tasks& tasks, SOCKET left, SOCKET right);
bool Authenticate(SOCKET socket, const std::string& token, bool controller);
}
