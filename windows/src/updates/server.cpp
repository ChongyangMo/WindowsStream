#include "server.h"
#include "../../shared/crypto.h"
#include "../../shared/net_health.h"
#include <map>
#include <sstream>
#include <filesystem>

namespace {
bool SendAll(SOCKET socket, const void* data, size_t length) {
    auto* cursor = static_cast<const char*>(data);
    while (length) {
        const int sent = send(socket, cursor, static_cast<int>(length), 0);
        if (sent <= 0) return false;
        cursor += sent; length -= sent;
    }
    return true;
}
void Respond(SOCKET socket, int code, const std::vector<uint8_t>& body = {}) {
    const auto header = "HTTP/1.1 " + std::to_string(code) + " Result\r\nContent-Type: application/octet-stream\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
    if (SendAll(socket, header.data(), header.size()) && !body.empty()) SendAll(socket, body.data(), body.size());
}
}

UpdateServer::~UpdateServer() { Stop(); }
void UpdateServer::StartConfigured(const std::wstring& config, const std::wstring& bind, const std::string& token) {
    if (!GetPrivateProfileIntW(L"updates", L"enabled", 1, config.c_str()) || token.size() < 32) return;
    const UINT port = GetPrivateProfileIntW(L"updates", L"port", 7402, config.c_str());
    if (!port || port > 65535) { LogLine("升级服务端口配置无效"); return; }
    wchar_t buffer[32768]{};
    GetPrivateProfileStringW(L"updates", L"package", L"..\\host\\WindowsStreamHost.exe", buffer, 32768, config.c_str());
    std::filesystem::path package(buffer);
    if (package.is_relative()) package = std::filesystem::path(config).parent_path() / package;
    Start(bind, static_cast<uint16_t>(port), token, package.lexically_normal().wstring());
}
void UpdateServer::Stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}
void UpdateServer::Start(const std::wstring& bindAddress, uint16_t port, const std::string& token, const std::wstring& package) {
    token_ = token; packagePath_ = package; running_ = true;
    thread_ = std::thread([this, bindAddress, port] {
        std::string lastError;
        while (running_) {
            SOCKET listener = Listen(bindAddress, port, lastError);
            if (listener == INVALID_SOCKET) {
                for (int i = 0; i < 10 && running_; ++i) Sleep(200);
                continue;
            }
            LogLine("远程升级服务已启动：%s:%u", WideToUtf8(bindAddress).c_str(), port);
            lastError.clear();
            net::AddressWatch watch(bindAddress);
            while (running_) {
                if (watch.Lost()) { LogLine("远程升级服务监听地址已移除，等待恢复"); break; }
                fd_set readable;
                FD_ZERO(&readable); FD_SET(listener, &readable);
                timeval timeout{0, 200000};
                const int selected = select(0, &readable, nullptr, nullptr, &timeout);
                if (selected == SOCKET_ERROR) { LogLine("远程升级服务监听中断：%d，重新监听", WSAGetLastError()); break; }
                if (!selected) continue;
                sockaddr_in from{};
                int fromLength = sizeof from;
                SOCKET peer = accept(listener, reinterpret_cast<sockaddr*>(&from), &fromLength);
                if (peer == INVALID_SOCKET) continue;
                if (!net::PeerAllowed(bindAddress, from)) { closesocket(peer); continue; }
                DWORD milliseconds = 5000;
                setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&milliseconds), sizeof milliseconds);
                setsockopt(peer, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&milliseconds), sizeof milliseconds);
                Handle(peer);
                closesocket(peer);
            }
            closesocket(listener);
        }
    });
}
SOCKET UpdateServer::Listen(const std::wstring& bindAddress, uint16_t port, std::string& lastError) {
    sockaddr_in address{};
    address.sin_family = AF_INET; address.sin_port = htons(port);
    std::string error;
    SOCKET listener = INVALID_SOCKET;
    if (InetPtonW(AF_INET, net::ListenAddress(bindAddress).c_str(), &address.sin_addr) != 1) error = "升级服务监听地址必须是 IPv4 地址或 auto";
    else if (!net::LocalAddressReady(bindAddress)) error = net::BindError(bindAddress, port, WSAEADDRNOTAVAIL);
    else if ((listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == INVALID_SOCKET) error = "无法创建升级服务套接字";
    else {
        BOOL exclusive = TRUE;
        setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof exclusive);
        if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0 && listen(listener, 4) == 0) return listener;
        error = net::BindError(bindAddress, port, WSAGetLastError());
        closesocket(listener);
    }
    if (error != lastError) LogLine("远程升级服务暂不可用：%s，稍后自动重试", error.c_str());
    lastError = error;
    return INVALID_SOCKET;
}
void UpdateServer::Handle(SOCKET peer) {
    std::string request;
    size_t boundary = std::string::npos;
    while ((boundary = request.find("\r\n\r\n")) == std::string::npos) {
        char bytes[2048];
        const int count = recv(peer, bytes, sizeof bytes, 0);
        if (count <= 0) return;
        request.append(bytes, count);
        if (request.size() > 8192) { Respond(peer, 400); return; }
    }
    std::istringstream lines(request.substr(0, boundary));
    std::string method, path, protocol, line;
    lines >> method >> path >> protocol;
    std::getline(lines, line);
    std::map<std::string, std::string> headers;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto separator = line.find(':');
        if (separator == std::string::npos) continue;
        auto name = line.substr(0, separator), value = line.substr(separator + 1);
        for (auto& c : name) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        value.erase(0, value.find_first_not_of(" \t"));
        headers[name] = value;
    }
    uint64_t length = 0;
    if (headers.count("content-length")) {
        const auto& text = headers["content-length"];
        if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos || text.size() > 6) { Respond(peer, 400); return; }
        length = strtoull(text.c_str(), nullptr, 10);
    }
    if (length > 65536) { Respond(peer, 413); return; }
    std::vector<uint8_t> body(request.begin() + boundary + 4, request.end());
    if (body.size() > length) { Respond(peer, 400); return; }
    while (body.size() < length) {
        uint8_t bytes[4096];
        const int count = recv(peer, reinterpret_cast<char*>(bytes), static_cast<int>(std::min<size_t>(sizeof bytes, length - body.size())), 0);
        if (count <= 0) return;
        body.insert(body.end(), bytes, bytes + count);
    }
    const auto& nonce = headers["x-ws-nonce"];
    const auto proof = updates::RequestProof(token_, method, path, nonce, body);
    const auto& suppliedProof = headers["x-ws-proof"];
    if (nonce.size() != 32 || nonce.find_first_not_of("0123456789abcdef") != std::string::npos || proof.size() != 64 || suppliedProof.size() != 64 ||
        !SecureEqual(reinterpret_cast<const uint8_t*>(proof.data()), reinterpret_cast<const uint8_t*>(suppliedProof.data()), 64)) {
        Respond(peer, 403); return;
    }
    if (method == "POST" && path == "/diagnostics") {
        std::istringstream entries(std::string(body.begin(), body.end()));
        while (std::getline(entries, line)) if (!line.empty()) LogLine("被控端诊断：%s", line.c_str());
        Respond(peer, 200); return;
    }
    if (method == "GET" && path == "/manifest") {
        updates::Package next;
        if (!updates::LoadPackage(packagePath_, next)) { Respond(peer, 503); return; }
        package_ = std::move(next);
        Respond(peer, 200, updates::Manifest(package_, token_)); return;
    }
    if (method == "GET" && path == "/package/" + updates::Hex(package_.hash.data(), 32) && !package_.bytes.empty()) {
        LogLine("向被控端提供升级包：%zu 字节", package_.bytes.size());
        Respond(peer, 200, package_.bytes); return;
    }
    Respond(peer, 404);
}
