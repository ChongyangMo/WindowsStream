#pragma once
#include "../../shared/proxy/transport.h"

namespace proxy {
// 固定 TCP 入口不解析应用协议，数据库握手和 TLS 字节直接进入已有认证通道。
class TcpForwards {
public:
    using Forward = std::function<void(const Peer&, Tasks&, const std::string&, uint16_t)>;
    ~TcpForwards() { Stop(); }
    void Start(const std::wstring& ini, Forward forward);
    void Stop() { tasks_.Stop(); forward_ = {}; }
private:
    void ListenLoop(std::wstring section, uint16_t localPort, std::string host, uint16_t targetPort);
    Tasks tasks_;
    Forward forward_;
};
}
