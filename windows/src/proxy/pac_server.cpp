#include "pac_server.h"

namespace proxy {
bool PacServer::Start(const std::string& script) {
    Stop();
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) return false;
    winsock_ = true; tasks_.Start();
    auto listener = Listen(tasks_, L"127.0.0.1", 0);
    if (!listener) { Stop(); return false; }
    sockaddr_in address{}; int length = sizeof address;
    if (getsockname(listener->value, reinterpret_cast<sockaddr*>(&address), &length)) { Stop(); return false; }
    const auto path = "/windowsstream.pac?revision=" + std::to_string(NowMicros());
    url_ = L"http://127.0.0.1:" + std::to_wstring(ntohs(address.sin_port)) + Utf8ToWide(path);
    if (!tasks_.Run([this, listener, path, script] {
        while (tasks_.running) {
            if (!Ready(listener->value, false, 200)) continue;
            auto peer = tasks_.Track(accept(listener->value, nullptr, nullptr));
            if (!peer) continue;
            Timeout(peer->value, 1000);
            std::string header; const auto deadline = GetTickCount64() + 2000;
            while (tasks_.running && header.size() < 8192 && GetTickCount64() < deadline && !header.ends_with("\r\n\r\n")) {
                char byte = 0; if (!Read(peer->value, &byte, 1)) break; header += byte;
            }
            const bool valid = header.ends_with("\r\n\r\n") && header.starts_with("GET " + path + " HTTP/1.");
            const auto body = valid ? script : "";
            const auto response = std::string(valid ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.1 404 Not Found\r\n") +
                "Content-Type: application/x-ns-proxy-autoconfig\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: " +
                std::to_string(body.size()) + "\r\n\r\n" + body;
            Text(peer->value, response);
        }
    })) { Stop(); return false; }
    return true;
}
void PacServer::Stop() {
    tasks_.Stop(); url_.clear();
    if (winsock_) { WSACleanup(); winsock_ = false; }
}
}
