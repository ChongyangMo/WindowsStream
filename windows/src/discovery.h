#pragma once

#include "common.h"

struct DiscoveredHost {
    std::wstring address;
    uint16_t port = 0;
    std::wstring name;
    bool wireless = false;
    int replyMs = 0;
};

// 查找 WindowsStream（UDP 7401），只接受能用 token 证明身份的应答。三种探测同时进行：
// 每块网卡发组播（239.255.77.87，不受子网掩码影响）、本网段广播，以及单播询问已知地址。
// 结果按“有线优先、应答快优先”排序；同一台 Windows 在多个网段上的地址都会列出。
std::vector<DiscoveredHost> DiscoverHosts(const std::string& token, const std::vector<std::wstring>& knownHosts, int timeoutMs);
