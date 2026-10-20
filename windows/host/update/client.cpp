#include "client.h"
#include "apply.h"
#include "../../shared/crypto.h"
#include "../locate/locator.h"
#include <winhttp.h>
#include <filesystem>
#include <fstream>

namespace {
bool Request(const HostConfig& config, const std::string& method, const std::string& path,
             const std::vector<uint8_t>& body, std::vector<uint8_t>& output, size_t limit) {
    HINTERNET session = WinHttpOpen(L"WindowsStreamUpdate/1", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 2000, 2000, 3000, 5000);
    const auto host = config.updateHost.empty() ? Locator().Current() : config.updateHost;
    HINTERNET connection = WinHttpConnect(session, host.c_str(), config.updatePort, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, Utf8ToWide(method).c_str(), Utf8ToWide(path).c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : nullptr;
    const auto nonce = updates::Nonce();
    const auto proof = updates::RequestProof(config.token, method, path, nonce, body);
    const auto headers = Utf8ToWide("X-WS-Nonce: " + nonce + "\r\nX-WS-Proof: " + proof + "\r\n");
    bool ok = request && !nonce.empty() && !proof.empty() && WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(headers.size()),
        body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<uint8_t*>(body.data()), static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) && WinHttpReceiveResponse(request, nullptr);
    DWORD status = 0, size = sizeof status;
    ok = ok && WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                  &status, &size, WINHTTP_NO_HEADER_INDEX) && status == 200;
    output.clear();
    while (ok) {
        uint8_t bytes[16384];
        DWORD read = 0;
        if (!WinHttpReadData(request, bytes, sizeof bytes, &read)) { ok = false; break; }
        if (!read) break;
        if (output.size() + read > limit) { ok = false; break; }
        output.insert(output.end(), bytes, bytes + read);
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return ok;
}
}

HostUpdateClient::~HostUpdateClient() { Stop(); }
void HostUpdateClient::Stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}
void HostUpdateClient::Start(const HostConfig& config, const std::string& bootId) {
    if (!config.updatesEnabled || (config.updateHost.empty() && config.controllerHost.empty())) return;
    running_ = true;
    thread_ = std::thread([this, config, bootId] {
        LogLine("自动升级检查已启动，升级连接独立于视频连接");
        while (running_) {
            Check(config, bootId);
            for (unsigned i = 0; i < config.updateSeconds * 10 && running_; ++i) Sleep(100);
        }
    });
}
void HostUpdateClient::Check(const HostConfig& config, const std::string& bootId) {
    std::vector<uint8_t> manifest;
    if (!Request(config, "GET", "/manifest", {}, manifest, 1024)) return;
    uint64_t size = 0;
    uint32_t stamp = 0;
    std::array<uint8_t, 32> expected{};
    if (!updates::VerifyManifest(manifest, config.token, size, stamp, expected)) { LogLine("升级清单身份校验失败"); return; }
    updates::MarkHealthy(bootId);
    const auto root = std::filesystem::path(updates::ModulePath()).parent_path();
    // 诊断增量独立上传，即使视频初始化失败仍可在 A 查看具体错误。
    const auto log = root / L"host.log";
    std::error_code error;
    const auto length = std::filesystem::file_size(log, error);
    if (!error && length != logOffset_) {
        if (length < logOffset_ || !logOffset_) logOffset_ = length > 65536 ? length - 65536 : 0;
        std::ifstream input(log, std::ios::binary);
        input.seekg(static_cast<std::streamoff>(logOffset_));
        std::vector<uint8_t> data(static_cast<size_t>(std::min<uint64_t>(65536, length - logOffset_)));
        input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
        data.resize(static_cast<size_t>(input.gcount()));
        std::vector<uint8_t> response;
        if (!data.empty() && Request(config, "POST", "/diagnostics", data, response, 1024)) logOffset_ += data.size();
    }
    updates::Package current;
    if (!updates::LoadPackage(updates::ModulePath(), current)) return;
    if (stamp <= current.stamp || SecureEqual(expected.data(), current.hash.data(), 32)) return;
    wchar_t rejected[80]{};
    GetPrivateProfileStringW(L"update", L"rejected_sha256", L"", rejected, 80, (root / L"update-state.ini").c_str());
    const auto hash = updates::Hex(expected.data(), expected.size());
    if (WideToUtf8(rejected) == hash) return;
    updates::Package candidate;
    if (!Request(config, "GET", "/package/" + hash, {}, candidate.bytes, updates::kMaxPackage) || candidate.bytes.size() != size ||
        !updates::Inspect(candidate) || candidate.stamp != stamp || !SecureEqual(candidate.hash.data(), expected.data(), 32)) {
        LogLine("升级包下载或完整性校验失败，保留当前程序"); return;
    }
    LogLine("新版程序已下载并验证，准备自动替换和重启");
    if (updates::Stage(root, GetCurrentProcessId(), candidate, manifest, config.token)) running_ = false;
    else LogLine("准备自动升级失败：%lu", GetLastError());
}
