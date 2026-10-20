#pragma once

#include "common.h"

struct ServerInfo {
    int width = 0;
    int height = 0;
    int fps = 0;
};

struct ClientDisplay {
    uint16_t viewWidth = 0, viewHeight = 0;        // 控制端客户区尺寸，首版被控端不据此重建画面
    uint16_t monitorWidth = 0, monitorHeight = 0;  // 控制端显示器模式，保留协议字段
    uint16_t refresh = 0;                          // 显示器刷新率（Hz）
};

bool HmacSha256(const std::string& key, const uint8_t* data, size_t size, uint8_t out[32]);
// 到目标地址的最佳路由是否经过 Wi-Fi 网卡。
bool RouteIsWireless(const sockaddr* address);

// 与 Windows 端的一条 TCP 连接。Read 只在网络线程调用；Send 可在任意线程调用。
class Connection {
public:
    Connection() = default;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    ~Connection();

    bool Connect(const std::wstring& host, uint16_t port, const std::string& token, const ClientDisplay& display,
                 ServerInfo& info, std::wstring& error);
    bool AcceptReverse(const std::wstring& bindHost, uint16_t port, const std::string& token,
                       const ClientDisplay& display, ServerInfo& info, std::wstring& error);
    static void CloseReverseListener();
    bool Read(uint8_t& type, std::vector<uint8_t>& payload);
    bool Send(uint8_t type, const void* data = nullptr, size_t size = 0);
    void Shutdown();
    bool Wireless() const { return wireless_; }  // 到 Windows 的路由是否走无线网卡

private:
    bool Handshake(const std::string& token, const ClientDisplay& display, ServerInfo& info, std::wstring& error);
    bool ReadExact(void* buffer, size_t size);

    SOCKET socket_ = INVALID_SOCKET;
    static SOCKET reverseListener_;  // 网络线程复用监听器，避免重连时反复绑定端口
    bool wireless_ = false;
    std::mutex sendMutex_;
};
