#pragma once
#include "../../shared/proxy/transport.h"

namespace proxy {
bool IntranetIPv4(const std::string& host);
uint32_t PacketCapabilities();
void RelayDatagrams(Tasks& tasks, SOCKET tunnel, const std::string& host, uint16_t port);
void RelayEcho(SOCKET tunnel, const std::string& host);
}
