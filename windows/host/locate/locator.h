#pragma once
#include "../config.h"

// 被控端当前使用的控制端地址。连接失败时在网线链路上广播寻找控制端，成功后记入 host.ini，
// 视频、代理、升级三条连接共用同一地址，控制端换了地址也能自动找回。
class ControllerLocator {
public:
    void Init(const HostConfig& config);
    std::wstring Current();
    // 主连接认证成功后调用，地址有变化时保存。
    void Succeeded();
    // 广播寻找控制端；找不到时在配置地址和上次成功地址之间轮换。按失败次数退避，不频繁广播。
    void Find(const std::atomic<bool>& running);
private:
    std::mutex mutex_;
    std::wstring ini_, configured_, saved_, current_;
    std::string token_;
    ULONGLONG nextFind_ = 0;
    unsigned misses_ = 0;
};

ControllerLocator& Locator();
// 单次寻找：169.254 网卡广播并单播询问 known，返回首个签名正确的应答来源地址。
std::wstring FindController(const std::string& token, const std::vector<std::wstring>& known, const std::atomic<bool>& running);
