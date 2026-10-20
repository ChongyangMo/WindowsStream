#include "responder.h"
#include "../../shared/locate.h"
#include "../../shared/net_health.h"

void LocateResponder::Stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void LocateResponder::Start(const std::wstring& bind, uint16_t port, const std::string& token) {
    if (token.size() < 32) return;
    running_ = true;
    thread_ = std::thread([this, bind, port, token] {
        bool reported = false;
        ULONG lastPeer = 0;
        ULONGLONG lastLogged = 0;
        while (running_) {
            SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_port = htons(locate::kPort);
            local.sin_addr.s_addr = INADDR_ANY;  // 广播只能在全部地址上收到，来源由口令签名和网段检查约束
            if (socket == INVALID_SOCKET || ::bind(socket, reinterpret_cast<sockaddr*>(&local), sizeof local) != 0) {
                if (!reported) LogLine("自动发现应答暂不可用：%d，稍后自动重试", WSAGetLastError());
                reported = true;
                if (socket != INVALID_SOCKET) closesocket(socket);
                for (int i = 0; i < 25 && running_; ++i) Sleep(200);
                continue;
            }
            LogLine("自动发现应答已启动，UDP %u", locate::kPort);
            reported = false;
            while (running_) {
                fd_set readable;
                FD_ZERO(&readable); FD_SET(socket, &readable);
                timeval wait{0, 250000};
                const int selected = select(0, &readable, nullptr, nullptr, &wait);
                if (selected == SOCKET_ERROR) break;
                if (!selected) continue;
                uint8_t request[128]{};
                sockaddr_in from{};
                int fromLength = sizeof from;
                const int received = recvfrom(socket, reinterpret_cast<char*>(request), sizeof request, 0,
                                              reinterpret_cast<sockaddr*>(&from), &fromLength);
                if (received <= 0 || !net::PeerAllowed(bind, from)) continue;
                const auto answer = locate::Answer(token, request, static_cast<size_t>(received), port);
                if (answer.empty()) continue;
                sendto(socket, reinterpret_cast<const char*>(answer.data()), static_cast<int>(answer.size()), 0,
                       reinterpret_cast<const sockaddr*>(&from), fromLength);
                // 同一来源一分钟内只记录一次，避免被控端反复寻找时刷屏。
                const auto now = GetTickCount64();
                if (from.sin_addr.s_addr != lastPeer || now >= lastLogged + 60000) {
                    wchar_t text[INET_ADDRSTRLEN]{};
                    InetNtopW(AF_INET, &from.sin_addr, text, INET_ADDRSTRLEN);
                    LogLine("已应答被控端自动发现：%s", WideToUtf8(text).c_str());
                    lastPeer = from.sin_addr.s_addr;
                    lastLogged = now;
                }
            }
            closesocket(socket);
        }
    });
}
