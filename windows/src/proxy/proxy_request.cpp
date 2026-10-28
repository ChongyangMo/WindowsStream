#include "request.h"
#include <sstream>

namespace proxy {
namespace {
bool Authority(const std::string& authority, uint16_t defaultPort, Request& request) {
    std::string host = authority, port;
    if (host.empty() || host.find_first_of("@/\\?# \t\r\n") != std::string::npos) return false;
    if (host.front() == '[') {
        auto end = host.find(']');
        if (end == std::string::npos) return false;
        if (end + 1 < host.size()) {
            if (host[end + 1] != ':') return false;
            port = host.substr(end + 2);
            if (port.empty()) return false;
        }
        host = host.substr(1, end - 1);
    } else if (auto colon = host.find(':'); colon != std::string::npos) {
        port = host.substr(colon + 1); host.resize(colon);
        if (port.empty()) return false;
    }
    unsigned number = defaultPort;
    if (!port.empty()) {
        if (port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos) return false;
        number = static_cast<unsigned>(strtoul(port.c_str(), nullptr, 10));
    }
    if (host.empty() || host.size() > 253 || !number || number > 65535 ||
        std::any_of(host.begin(), host.end(), [](unsigned char c) { return c <= 32 || c >= 127; })) return false;
    request.host = host; request.port = static_cast<uint16_t>(number); return true;
}
bool Socks(SOCKET socket, Request& request) {
    request.socks = true;
    uint8_t count = 0;
    if (!Read(socket, &count, 1) || !count) return false;
    std::vector<uint8_t> methods(count);
    if (!Read(socket, methods.data(), count)) return false;
    const uint8_t method[2]{5, static_cast<uint8_t>(std::find(methods.begin(), methods.end(), 0) == methods.end() ? 255 : 0)};
    if (!Send(socket, method, 2) || method[1] == 255) return false;
    uint8_t header[4]{};
    if (!Read(socket, header, 4)) return false;
    if (header[0] != 5 || header[2] != 0) { Reply(socket, request, 1); return false; }
    if (header[1] != 1) { Reply(socket, request, 7); return false; }
    char address[INET6_ADDRSTRLEN]{};
    if (header[3] == 1 || header[3] == 4) {
        uint8_t bytes[16]{};
        if (!Read(socket, bytes, header[3] == 1 ? 4 : 16) ||
            !inet_ntop(header[3] == 1 ? AF_INET : AF_INET6, bytes, address, sizeof address)) return false;
        request.host = address;
    } else if (header[3] == 3) {
        uint8_t length = 0;
        if (!Read(socket, &length, 1) || !length) return false;
        request.host.resize(length);
        if (!Read(socket, request.host.data(), length) ||
            std::any_of(request.host.begin(), request.host.end(), [](unsigned char c) { return c <= 32 || c >= 127; })) return false;
    } else { Reply(socket, request, 8); return false; }
    uint8_t port[2]{};
    if (!Read(socket, port, 2)) return false;
    request.port = static_cast<uint16_t>(port[0] * 256 + port[1]);
    if (!request.port) { Reply(socket, request, 1); return false; }
    return true;
}
bool Http(SOCKET socket, uint8_t first, Request& request) {
    std::string header(1, static_cast<char>(first));
    const auto deadline = GetTickCount64() + 5000;
    while (!header.ends_with("\r\n\r\n")) {
        char byte = 0;
        if (header.size() >= 32768 || GetTickCount64() >= deadline || !Read(socket, &byte, 1) || byte == '\0') return false;
        header += byte;
    }
    std::istringstream lines(header);
    std::string method, target, version, line;
    if (!(lines >> method >> target >> version) || (version != "HTTP/1.1" && version != "HTTP/1.0")) return false;
    std::getline(lines, line);
    if (method == "GET" && (target == "/windowsstream.pac" || target.starts_with("/windowsstream.pac?"))) {
        request.pac = true; return true;
    }
    request.connect = method == "CONNECT";
    if (request.connect) return Authority(target, 443, request);
    if (!target.starts_with("http://")) return false;
    auto end = target.find_first_of("/?#", 7);
    auto authority = target.substr(7, end == std::string::npos ? end : end - 7);
    if (!Authority(authority, 80, request)) return false;
    std::string path = end == std::string::npos ? "/" : target.substr(end);
    if (path.front() != '/') path = "/" + path;
    if (auto fragment = path.find('#'); fragment != std::string::npos) path.resize(fragment);
    request.initial = method + " " + path + " " + version + "\r\nHost: " + authority + "\r\n";
    std::string fields;
    bool upgrade = false;
    while (std::getline(lines, line) && line != "\r") {
        if (line.empty() || line.back() != '\r' || line.front() == ' ' || line.front() == '\t') return false;
        line.pop_back();
        const auto colon = line.find(':');
        if (colon == std::string::npos) return false;
        auto name = line.substr(0, colon);
        for (auto& c : name) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (name == "upgrade") upgrade = true;
        if (name != "host" && name != "proxy-authorization" && name != "proxy-connection" && name != "connection") fields += line + "\r\n";
    }
    request.initial += fields + (upgrade ? "Connection: Upgrade\r\n\r\n" : "Connection: close\r\n\r\n");
    return true;
}
}
bool Parse(SOCKET socket, Request& request) {
    uint8_t first = 0;
    if (!Read(socket, &first, 1)) return false;
    if (first == 127) {
        request.native = true;
        if (!Read(socket, &request.command, 1) || request.command < 1 || request.command > 4) return false;
        if (request.command == 4) return true;
        uint8_t header[4]{};
        if (!Read(socket, header, 4)) return false;
        request.port = ReadU16(header);
        const auto size = ReadU16(header + 2);
        if ((!request.port && request.command != 3) || !size || size > 15) return false;
        request.host.resize(size);
        if (!Read(socket, request.host.data(), size)) return false;
        IN_ADDR address{};
        if (inet_pton(AF_INET, request.host.c_str(), &address) != 1 || (ntohl(address.s_addr) & 0xffff0000u) != 0x0a120000u) {
            Reply(socket, request, 2); return false;
        }
        return true;
    }
    if (first == 5) return Socks(socket, request);
    if (Http(socket, first, request)) return true;
    Text(socket, "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"); return false;
}
bool Reply(SOCKET socket, const Request& request, uint8_t error) {
    // 固定 TCP 入口不能向数据库发送 HTTP/SOCKS 错误字节，失败时直接关闭。
    if (request.raw) return error == 0;
    if (request.native) return Send(socket, &error, 1);
    if (request.socks) {
        const uint8_t bytes[10]{5, error, 0, 1, 0, 0, 0, 0, 0, 0};
        return Send(socket, bytes, sizeof bytes);
    }
    if (error) return Text(socket, "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    return !request.connect || Text(socket, "HTTP/1.1 200 Connection Established\r\n\r\n");
}
}
