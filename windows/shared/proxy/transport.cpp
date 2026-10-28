#include "transport.h"
#include "../net_health.h"
#include <array>

namespace proxy {
Peer Tasks::Track(SOCKET socket) {
    if (socket == INVALID_SOCKET) return {};
    auto peer = std::make_shared<Socket>(socket);
    std::lock_guard lock(mutex_);
    if (!running) return {};
    std::erase_if(sockets_, [](const auto& item) { return item.expired(); });
    sockets_.push_back(peer);
    return peer;
}
bool Tasks::Watch(const Peer& peer) {
    std::lock_guard lock(mutex_);
    if (!running || !peer) return false;
    std::erase_if(sockets_, [](const auto& item) { return item.expired(); });
    sockets_.push_back(peer);
    return true;
}
bool Tasks::Run(std::function<void()> work) {
    std::lock_guard lock(mutex_);
    if (!running) return false;
    std::erase_if(tasks_, [](Task& task) {
        if (!task.done->load()) return false;
        task.thread.join(); return true;
    });
    if (tasks_.size() >= 100) return false;
    auto done = std::make_shared<std::atomic<bool>>(false);
    tasks_.push_back({std::thread([work = std::move(work), done] {
        try { work(); } catch (...) { LogLine("代理任务异常结束"); }
        *done = true;
    }), done});
    return true;
}
void Tasks::Stop() {
    std::vector<Task> tasks;
    {
        std::lock_guard lock(mutex_);
        running = false;
        for (const auto& item : sockets_) if (auto socket = item.lock()) shutdown(socket->value, SD_BOTH);
        tasks.swap(tasks_);
    }
    for (auto& task : tasks) if (task.thread.joinable()) task.thread.join();
    std::lock_guard lock(mutex_);
    sockets_.clear();
}
void Timeout(SOCKET socket, DWORD milliseconds) {
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&milliseconds), sizeof milliseconds);
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&milliseconds), sizeof milliseconds);
    // 空闲反向通道没有数据往来，网线拔插后只能靠短周期保活发现失效并重连。
    net::KeepAlive(socket);
    BOOL enabled = TRUE;
    setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&enabled), sizeof enabled);
}
bool Read(SOCKET socket, void* data, size_t size) {
    auto cursor = static_cast<char*>(data);
    while (size) {
        int count = recv(socket, cursor, static_cast<int>(std::min<size_t>(size, 65536)), 0);
        if (count <= 0) return false;
        cursor += count; size -= count;
    }
    return true;
}
bool Send(SOCKET socket, const void* data, size_t size) {
    auto cursor = static_cast<const char*>(data);
    while (size) {
        int count = send(socket, cursor, static_cast<int>(std::min<size_t>(size, 65536)), 0);
        if (count <= 0) return false;
        cursor += count; size -= count;
    }
    return true;
}
bool Text(SOCKET socket, const std::string& text) { return Send(socket, text.data(), text.size()); }
bool Ready(SOCKET socket, bool write, unsigned milliseconds) {
    fd_set ready; FD_ZERO(&ready); FD_SET(socket, &ready);
    timeval wait{static_cast<long>(milliseconds / 1000), static_cast<long>(milliseconds % 1000 * 1000)};
    return select(0, write ? nullptr : &ready, write ? &ready : nullptr, nullptr, &wait) > 0;
}
Peer Listen(Tasks& tasks, const std::wstring& host, uint16_t port, int* error) {
    if (error) *error = 0;
    auto peer = tasks.Track(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!peer) { if (error) *error = WSAGetLastError(); return {}; }
    BOOL exclusive = TRUE;
    setsockopt(peer->value, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof exclusive);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port);
    if (InetPtonW(AF_INET, host.c_str(), &address.sin_addr) != 1) { if (error) *error = WSAEINVAL; return {}; }
    if (bind(peer->value, reinterpret_cast<sockaddr*>(&address), sizeof address) || listen(peer->value, 32)) {
        if (error) *error = WSAGetLastError();
        return {};
    }
    return peer;
}
Peer Connect(Tasks& tasks, const std::wstring& host, uint16_t port, uint8_t& error) {
    error = 4;
    ADDRINFOEXW hints{}; hints.ai_socktype = SOCK_STREAM; hints.ai_protocol = IPPROTO_TCP;
    ADDRINFOEXW* addresses = nullptr;
    // 域名在实际连接目标的一端解析；代理入口不查询本机 DNS。
    const auto deadline = GetTickCount64() + 6000;
    OVERLAPPED lookup{}; lookup.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!lookup.hEvent) return {};
    HANDLE cancel = nullptr;
    const auto service = std::to_wstring(port);
    int resolved = GetAddrInfoExW(host.c_str(), service.c_str(), NS_ALL, nullptr, &hints, &addresses, nullptr, &lookup, nullptr, &cancel);
    if (resolved == WSA_IO_PENDING) {
        while (WaitForSingleObject(lookup.hEvent, 200) == WAIT_TIMEOUT) {
            if (!tasks.running || GetTickCount64() >= deadline) { GetAddrInfoExCancel(&cancel); break; }
        }
        // 取消完成前不能释放 OVERLAPPED 或结果指针。
        WaitForSingleObject(lookup.hEvent, INFINITE);
        resolved = GetAddrInfoExOverlappedResult(&lookup);
    }
    CloseHandle(lookup.hEvent);
    if (resolved) { if (addresses) FreeAddrInfoExW(addresses); return {}; }
    Peer connected;
    for (auto address = addresses; address && tasks.running && GetTickCount64() < deadline; address = address->ai_next) {
        auto peer = tasks.Track(socket(address->ai_family, SOCK_STREAM, IPPROTO_TCP));
        if (!peer) continue;
        u_long nonblocking = 1; ioctlsocket(peer->value, FIONBIO, &nonblocking);
        int result = connect(peer->value, address->ai_addr, static_cast<int>(address->ai_addrlen));
        if (result && WSAGetLastError() != WSAEWOULDBLOCK) continue;
        while (result && tasks.running && GetTickCount64() < deadline) {
            fd_set writable, failed; FD_ZERO(&writable); FD_ZERO(&failed);
            FD_SET(peer->value, &writable); FD_SET(peer->value, &failed);
            timeval wait{0, 200000};
            int selected = select(0, nullptr, &writable, &failed, &wait);
            if (selected == SOCKET_ERROR) break;
            if (!selected) continue;
            int code = 0, length = sizeof code;
            if (!getsockopt(peer->value, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&code), &length) && !code) result = 0;
            else error = code == WSAECONNREFUSED ? 5 : code == WSAENETUNREACH ? 3 : 4;
            break;
        }
        if (!result && tasks.running) {
            nonblocking = 0; ioctlsocket(peer->value, FIONBIO, &nonblocking);
            Timeout(peer->value, 10000); connected = peer; error = 0; break;
        }
    }
    FreeAddrInfoExW(addresses);
    return connected;
}

// 两个有界缓冲区，支持半关闭及慢接收方背压，不为每个方向再创建线程。
void Relay(Tasks& tasks, SOCKET left, SOCKET right) {
    struct Direction {
        SOCKET from, to;
        std::array<char, 65536> bytes{};
        size_t begin = 0, end = 0;
        bool eof = false, closed = false;
    };
    Direction directions[2]{{left, right}, {right, left}};
    u_long nonblocking = 1;
    ioctlsocket(left, FIONBIO, &nonblocking); ioctlsocket(right, FIONBIO, &nonblocking);
    while (tasks.running) {
        fd_set readable, writable; FD_ZERO(&readable); FD_ZERO(&writable);
        bool finished = true;
        for (auto& d : directions) {
            if (d.eof && d.begin == d.end && !d.closed) { shutdown(d.to, SD_SEND); d.closed = true; }
            if (!d.closed) finished = false;
            if (!d.eof && d.end == 0) FD_SET(d.from, &readable);
            if (d.begin != d.end) FD_SET(d.to, &writable);
        }
        if (finished) break;
        timeval wait{0, 200000};
        int selected = select(0, &readable, &writable, nullptr, &wait);
        if (selected == SOCKET_ERROR) break;
        for (auto& d : directions) {
            if (FD_ISSET(d.from, &readable)) {
                int count = recv(d.from, d.bytes.data(), static_cast<int>(d.bytes.size()), 0);
                if (count > 0) d.end = count;
                else if (!count) d.eof = true;
                else if (WSAGetLastError() != WSAEWOULDBLOCK) return;
            }
            if (FD_ISSET(d.to, &writable)) {
                int count = send(d.to, d.bytes.data() + d.begin, static_cast<int>(d.end - d.begin), 0);
                if (count > 0) { d.begin += count; if (d.begin == d.end) d.begin = d.end = 0; }
                else if (count == 0 || WSAGetLastError() != WSAEWOULDBLOCK) return;
            }
        }
    }
}
}
