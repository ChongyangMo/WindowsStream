#pragma once
#include "../../shared/proxy/transport.h"

namespace proxy {
struct Request {
    std::string host;
    uint16_t port = 0;
    bool socks = false, connect = false;
    bool native = false;
    uint8_t command = 1;
    std::string initial;
};
bool Parse(SOCKET socket, Request& request);
bool Reply(SOCKET socket, const Request& request, uint8_t error);
}
