#pragma once
#include "../../shared/proxy/transport.h"
#include <functional>

namespace intranet {
struct EchoResult {
    bool success = false;
    std::wstring message;
};
uint16_t ProxyPort(const std::wstring& ini);
bool ValidTarget(const std::wstring& target);
EchoResult Echo(proxy::Tasks& tasks, uint16_t port, const std::wstring& target);
void Ping(proxy::Tasks& tasks, uint16_t port, const std::wstring& target,
          const std::function<void(const std::wstring&)>& output);
}
