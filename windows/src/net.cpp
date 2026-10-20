#include "net.h"
#include "../shared/crypto.h"
#include "../shared/net_health.h"

#include <bcrypt.h>
#include <iphlpapi.h>
#include <netioapi.h>

SOCKET Connection::reverseListener_ = INVALID_SOCKET;

void Connection::CloseReverseListener() {
    if (reverseListener_ != INVALID_SOCKET) closesocket(reverseListener_);
    reverseListener_ = INVALID_SOCKET;
}

bool RouteIsWireless(const sockaddr* address) {
    DWORD index = 0;
    if (GetBestInterfaceEx(const_cast<sockaddr*>(address), &index) != NO_ERROR) return false;
    MIB_IF_ROW2 row{};
    row.InterfaceIndex = index;
    return GetIfEntry2(&row) == NO_ERROR && row.Type == IF_TYPE_IEEE80211;
}

Connection::~Connection() {
    if (socket_ != INVALID_SOCKET) {
        closesocket(socket_);
    }
}

bool Connection::Connect(const std::wstring& host, uint16_t port, const std::string& token, const ClientDisplay& display,
                         ServerInfo& info, std::wstring& error) {
    ADDRINFOW hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    ADDRINFOW* result = nullptr;
    if (GetAddrInfoW(host.c_str(), std::to_wstring(port).c_str(), &hints, &result) != 0 || result == nullptr) {
        error = L"无法解析地址 " + host;
        return false;
    }
    wireless_ = RouteIsWireless(result->ai_addr);
    socket_ = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (socket_ == INVALID_SOCKET) {
        FreeAddrInfoW(result);
        error = L"创建套接字失败";
        return false;
    }

    // 非阻塞连接 + 3 秒超时，之后切回阻塞模式。
    u_long nonBlocking = 1;
    ioctlsocket(socket_, FIONBIO, &nonBlocking);
    connect(socket_, result->ai_addr, static_cast<int>(result->ai_addrlen));
    FreeAddrInfoW(result);
    fd_set writable, failed;
    FD_ZERO(&writable);
    FD_ZERO(&failed);
    FD_SET(socket_, &writable);
    FD_SET(socket_, &failed);
    timeval timeout{3, 0};
    if (select(0, nullptr, &writable, &failed, &timeout) <= 0 || FD_ISSET(socket_, &failed)) {
        error = L"连接 " + host + L":" + std::to_wstring(port) + L" 失败（Windows 端 WindowsStream 是否在运行？）";
        return false;
    }
    nonBlocking = 0;
    ioctlsocket(socket_, FIONBIO, &nonBlocking);
    net::KeepAlive(socket_);

    int socketError = 0;
    int errorLength = sizeof socketError;
    if (getsockopt(socket_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socketError), &errorLength) != 0 || socketError) {
        error = L"连接被控端失败";
        return false;
    }
    return Handshake(token, display, info, error);
}

bool Connection::AcceptReverse(const std::wstring& bindHost, uint16_t port, const std::string& token,
                                const ClientDisplay& display, ServerInfo& info, std::wstring& error) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (InetPtonW(AF_INET, net::ListenAddress(bindHost).c_str(), &address.sin_addr) != 1) {
        error = L"监听地址必须是 IPv4 地址或 auto";
        return false;
    }
    if (net::AutoBind(bindHost)) {
        // 自动模式下网线链路或 169.254 地址缺失时被控端的包无法到达，状态变化时提示一次。
        // 链路断开（对端断电、睡眠、扩展坞失电）与本机缺地址分开，避免误导去重跑 setup-link。
        static ULONGLONG nextCheck = 0;
        static int linkState = -1, lastSeen = -1;
        if (GetTickCount64() >= nextCheck) {
            nextCheck = GetTickCount64() + 5000;
            const int state = net::HasLinkLocalAddress() ? 2 : net::WiredLinkUp() ? 1 : 0;
            // 链路刚恢复时地址还在冲突检测，连续两次相同才提示，避免短暂误报缺地址。
            const bool stable = state == lastSeen;
            lastSeen = state;
            if (stable && state != linkState) {
                static const char* const text[] = {
                    "本机有线网卡没有链路：被控端可能已断电、睡眠，或网线/扩展坞未供电；链路恢复后被控端会自动连入",
                    "网线已接通但本机没有 169.254 链路地址，被控端无法连入，请运行 setup-link.cmd",
                    "本机已有 169.254 链路地址，被控端可以通过网线连入"};
                uint64_t mbps = 0;
                if (state == 2 && net::WiredLinkUp(&mbps) && mbps) LogLine("%s，链路 %llu Mbps", text[state], mbps);
                else LogLine("%s", text[state]);
                linkState = state;
            }
        }
    }
    // 网线拔插或重启后监听地址可能消失；绑定在旧地址上的监听器不再可靠，地址恢复后重新绑定。
    static net::AddressWatch watch(bindHost);
    if (reverseListener_ != INVALID_SOCKET && watch.Lost()) {
        LogLine("监听地址 %s 已从本机网卡移除，关闭监听等待恢复", WideToUtf8(bindHost).c_str());
        CloseReverseListener();
    }
    if (reverseListener_ == INVALID_SOCKET) {
        if (!net::LocalAddressReady(bindHost)) {
            error = Utf8ToWide(net::BindError(bindHost, port, WSAEADDRNOTAVAIL));
            Sleep(1000);
            return false;
        }
        reverseListener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (reverseListener_ == INVALID_SOCKET) { error = L"无法创建监听套接字"; return false; }
        BOOL exclusive = TRUE;
        setsockopt(reverseListener_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof exclusive);
        if (bind(reverseListener_, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0 || listen(reverseListener_, 1) != 0) {
            error = Utf8ToWide(net::BindError(bindHost, port, WSAGetLastError()));
            CloseReverseListener();
            Sleep(1000);
            return false;
        }
        LogLine("已在 %s:%u 等待被控端连接", WideToUtf8(bindHost).c_str(), port);
    }
    fd_set readable;
    FD_ZERO(&readable); FD_SET(reverseListener_, &readable);
    timeval wait{1, 0};
    const int selected = select(0, &readable, nullptr, nullptr, &wait);
    if (selected == SOCKET_ERROR) {
        error = L"监听连接失败";
        CloseReverseListener();
        return false;
    }
    if (selected == 0) {
        error.clear();
        return false;
    }
    sockaddr_in peer{};
    int length = sizeof peer;
    socket_ = accept(reverseListener_, reinterpret_cast<sockaddr*>(&peer), &length);
    if (socket_ == INVALID_SOCKET) { error = L"接收连接失败"; return false; }
    if (!net::PeerAllowed(bindHost, peer)) {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
        error.clear();
        return false;
    }
    net::KeepAlive(socket_);
    wireless_ = RouteIsWireless(reinterpret_cast<const sockaddr*>(&peer));
    return Handshake(token, display, info, error);
}

bool Connection::Handshake(const std::string& token, const ClientDisplay& display,
                            ServerInfo& info, std::wstring& error) {

    BOOL noDelay = TRUE;
    setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof noDelay);
    int bufferSize = 8 * 1024 * 1024;
    setsockopt(socket_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bufferSize), sizeof bufferSize);
    DWORD handshakeTimeout = 5000;
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&handshakeTimeout), sizeof handshakeTimeout);
    setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&handshakeTimeout), sizeof handshakeTimeout);

    uint8_t type = 0;
    std::vector<uint8_t> payload;
    if (!Read(type, payload) || type != MsgServerHello || payload.size() != 68) {
        error = L"握手失败：服务端没有正确响应";
        return false;
    }
    uint32_t version = ReadU32(payload.data());
    if (version != kProtocolVersion) {
        error = L"协议版本不一致（Windows 端 " + std::to_wstring(version) + L"）";
        return false;
    }
    const std::string context = "windowsstream-host-v2";
    Writer proof;
    proof.append(context.data(), context.size());
    proof.append(payload.data() + 4, 32);
    uint8_t expected[32]{};
    if (!HmacSha256(token, proof.bytes.data(), proof.bytes.size(), expected) ||
        !SecureEqual(expected, payload.data() + 36, 32)) {
        error = L"被控端身份验证失败";
        return false;
    }
    uint8_t mac[32];
    if (!HmacSha256(token, payload.data() + 4, 32, mac)) {
        error = L"计算认证码失败";
        return false;
    }
    Writer auth;
    auth.append(mac, sizeof mac);
    auth.u32(kProtocolVersion);
    auth.u16(display.viewWidth);
    auth.u16(display.viewHeight);
    auth.u16(display.monitorWidth);
    auth.u16(display.monitorHeight);
    auth.u16(display.refresh);
    auth.u8(wireless_ ? 1 : 0);
    if (!Send(MsgClientAuth, auth.bytes.data(), auth.bytes.size())) {
        error = L"发送连接认证失败";
        return false;
    }

    if (!Read(type, payload) || type != MsgAuthResult || payload.empty() || payload[0] != 1 || payload.size() < 7) {
        error = L"认证失败：token 不匹配";
        return false;
    }
    info.width = ReadU16(payload.data() + 1);
    info.height = ReadU16(payload.data() + 3);
    info.fps = ReadU16(payload.data() + 5);

    DWORD receiveTimeout = 10000;
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&receiveTimeout), sizeof receiveTimeout);
    return true;
}

bool Connection::ReadExact(void* buffer, size_t size) {
    auto p = static_cast<char*>(buffer);
    while (size > 0) {
        int n = recv(socket_, p, static_cast<int>(std::min<size_t>(size, 1 << 20)), 0);
        if (n <= 0) {
            return false;
        }
        p += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

bool Connection::Read(uint8_t& type, std::vector<uint8_t>& payload) {
    uint8_t header[5];
    if (!ReadExact(header, sizeof header)) {
        return false;
    }
    uint32_t length = ReadU32(header);
    if (length > 32 * 1024 * 1024) {
        LogLine("消息过大：%u 字节", length);
        return false;
    }
    type = header[4];
    payload.resize(length);
    return length == 0 || ReadExact(payload.data(), length);
}

bool Connection::Send(uint8_t type, const void* data, size_t size) {
    std::vector<uint8_t> frame(5 + size);
    frame[0] = static_cast<uint8_t>(size);
    frame[1] = static_cast<uint8_t>(size >> 8);
    frame[2] = static_cast<uint8_t>(size >> 16);
    frame[3] = static_cast<uint8_t>(size >> 24);
    frame[4] = type;
    if (size > 0) {
        memcpy(frame.data() + 5, data, size);
    }
    std::lock_guard lock(sendMutex_);
    const char* p = reinterpret_cast<const char*>(frame.data());
    size_t remaining = frame.size();
    while (remaining > 0) {
        int n = send(socket_, p, static_cast<int>(remaining), 0);
        if (n <= 0) {
            return false;
        }
        p += n;
        remaining -= static_cast<size_t>(n);
    }
    return true;
}

void Connection::Shutdown() {
    if (socket_ != INVALID_SOCKET) {
        shutdown(socket_, SD_BOTH);
    }
}
