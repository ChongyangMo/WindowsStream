#include "channel.h"
#include "../shared/crypto.h"
#include "../shared/net_health.h"
#include <bcrypt.h>
#include <ws2tcpip.h>

SOCKET HostChannel::listener_ = INVALID_SOCKET;

void HostChannel::CloseListener() {
    if (listener_ != INVALID_SOCKET) closesocket(listener_);
    listener_ = INVALID_SOCKET;
}

namespace {
void SetTimeout(SOCKET socket, DWORD milliseconds) {
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&milliseconds), sizeof milliseconds);
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&milliseconds), sizeof milliseconds);
}
}

HostChannel::~HostChannel() {
    if (socket_ != INVALID_SOCKET) closesocket(socket_);
}

bool HostChannel::Open(const HostConfig& config, const std::wstring& controller, std::atomic<bool>& running) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(config.port);
    if (InetPtonW(AF_INET, (config.reverse ? controller : config.bindHost).c_str(), &address.sin_addr) != 1) {
        LogLine("连接地址必须是 IPv4 地址");
        return false;
    }
    if (config.reverse) {
        SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket == INVALID_SOCKET) return false;
        u_long nonBlocking = 1;
        ioctlsocket(socket, FIONBIO, &nonBlocking);
        int result = connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof address);
        if (result == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
            closesocket(socket);
            return false;
        }
        fd_set writable, failed;
        FD_ZERO(&writable); FD_ZERO(&failed);
        FD_SET(socket, &writable); FD_SET(socket, &failed);
        timeval wait{1, 0};
        int error = 0;
        int length = sizeof error;
        if (!running || select(0, nullptr, &writable, &failed, &wait) <= 0 ||
            FD_ISSET(socket, &failed) || getsockopt(socket, SOL_SOCKET, SO_ERROR,
            reinterpret_cast<char*>(&error), &length) != 0 || error != 0) {
            closesocket(socket);
            return false;
        }
        nonBlocking = 0;
        ioctlsocket(socket, FIONBIO, &nonBlocking);
        socket_ = socket;
    } else {
        if (listener_ == INVALID_SOCKET) {
            listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (listener_ == INVALID_SOCKET) return false;
            BOOL exclusive = TRUE;
            setsockopt(listener_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof exclusive);
            if (bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0 || listen(listener_, 1) != 0) {
                LogLine("监听端口失败：%d", WSAGetLastError());
                CloseListener();
                return false;
            }
        }
        while (running) {
            fd_set readable;
            FD_ZERO(&readable); FD_SET(listener_, &readable);
            timeval wait{0, 250000};
            const int selected = select(0, &readable, nullptr, nullptr, &wait);
            if (selected == SOCKET_ERROR) { CloseListener(); break; }
            if (selected > 0) {
                socket_ = accept(listener_, nullptr, nullptr);
                break;
            }
        }
    }
    if (socket_ == INVALID_SOCKET) return false;
    BOOL noDelay = TRUE;
    setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof noDelay);
    net::KeepAlive(socket_);
    SetTimeout(socket_, 5000);
    return true;
}

bool HostChannel::Authenticate(const std::string& token, int width, int height, int fps) {
    uint8_t nonce[32]{};
    if (BCryptGenRandom(nullptr, nonce, sizeof nonce, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return false;
    Writer hello;
    hello.u32(kProtocolVersion);
    hello.append(nonce, sizeof nonce);
    const std::string context = "windowsstream-host-v2";
    Writer proof;
    proof.append(context.data(), context.size());
    proof.append(nonce, sizeof nonce);
    uint8_t serverProof[32]{};
    if (!HmacSha256(token, proof.bytes.data(), proof.bytes.size(), serverProof)) return false;
    hello.append(serverProof, sizeof serverProof);
    if (!Send(MsgServerHello, hello)) return false;
    uint8_t type = 0;
    std::vector<uint8_t> payload;
    uint8_t expected[32]{};
    bool ok = Read(type, payload) && type == MsgClientAuth && payload.size() == 47 &&
              ReadU32(payload.data() + 32) == kProtocolVersion &&
              HmacSha256(token, nonce, sizeof nonce, expected) && SecureEqual(expected, payload.data(), 32);
    Writer result;
    result.u8(ok ? 1 : 0);
    result.u16(static_cast<uint16_t>(width));
    result.u16(static_cast<uint16_t>(height));
    result.u16(static_cast<uint16_t>(fps));
    if (!Send(MsgAuthResult, result) || !ok) {
        LogLine("连接认证失败");
        return false;
    }
    SetTimeout(socket_, 5000);
    return true;
}

bool HostChannel::ReadExact(void* data, size_t size) {
    auto* cursor = static_cast<char*>(data);
    while (size) {
        int received = recv(socket_, cursor, static_cast<int>(size), 0);
        if (received <= 0) return false;
        cursor += received;
        size -= received;
    }
    return true;
}

bool HostChannel::Read(uint8_t& type, std::vector<uint8_t>& payload) {
    uint8_t header[5]{};
    if (!ReadExact(header, sizeof header)) return false;
    const uint32_t length = ReadU32(header);
    if (length > 4 * 1024 * 1024) {
        LogLine("拒绝过大的控制消息：%u 字节", length);
        return false;
    }
    type = header[4];
    payload.resize(length);
    return !length || ReadExact(payload.data(), length);
}

bool HostChannel::Send(uint8_t type, const Writer& payload) {
    if (payload.bytes.size() > 32 * 1024 * 1024) return false;
    Writer frame;
    frame.u32(static_cast<uint32_t>(payload.bytes.size()));
    frame.u8(type);
    frame.append(payload.bytes.data(), payload.bytes.size());
    std::lock_guard lock(sendMutex_);
    const char* cursor = reinterpret_cast<const char*>(frame.bytes.data());
    size_t remaining = frame.bytes.size();
    while (remaining) {
        int sent = send(socket_, cursor, static_cast<int>(remaining), 0);
        if (sent <= 0) return false;
        cursor += sent;
        remaining -= sent;
    }
    return true;
}

void HostChannel::Shutdown() {
    if (socket_ != INVALID_SOCKET) shutdown(socket_, SD_BOTH);
}
