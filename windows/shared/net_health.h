#pragma once
#include "../src/common.h"

// 网线拔插、重启或地址被移除后，监听和长连接需要自行发现并恢复。
namespace net {
// 0.0.0.0 和回环地址总是可用；其他地址须当前存在于本机网卡且已完成冲突检测。
bool LocalAddressReady(const std::wstring& address);
// 已打开的监听器按间隔复查地址，避免每次轮询都枚举网卡。
class AddressWatch {
public:
    explicit AddressWatch(std::wstring address) : address_(std::move(address)) {}
    bool Lost();
private:
    std::wstring address_;
    ULONGLONG next_ = 0;
};
// 绑定失败原因，地址缺失与端口占用分开提示。
std::string BindError(const std::wstring& address, uint16_t port, int code);
// bind_host=auto：监听全部地址，只接受网线链路本地网段 169.254.0.0/16 的对端，本机地址变化不影响监听。
bool AutoBind(const std::wstring& bind);
std::wstring ListenAddress(const std::wstring& bind);
bool PeerAllowed(const std::wstring& bind, const sockaddr_in& peer);
// 本机是否有已生效的 169.254 地址；没有时被控端无法通过网线连入。
bool HasLinkLocalAddress();
// 本机物理有线网卡是否有链路，可选返回协商速率；对端断电、睡眠、扩展坞失电或网卡节能重协商时为 false。
bool WiredLinkUp(uint64_t* mbps = nullptr);
// 空闲约 10 秒开始探测，2 秒间隔，对端掉线后约半分钟内结束连接。
void KeepAlive(SOCKET socket);
}
