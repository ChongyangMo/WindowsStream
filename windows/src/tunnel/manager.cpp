#include "manager.h"
#include "../proxy/system_settings.h"
#include <shellapi.h>

void IpTunnel::Start(const std::wstring& ini) {
    // A 曾发生 0x3B 系统崩溃；在完成转储分析前禁止启动内核网卡相关功能。
    if (!attempted_) LogLine("IP 隧道因系统崩溃排查暂时禁用，未创建网卡或修改路由");
    attempted_ = true;
    return;
    if (attempted_) return;
    attempted_ = true;
    const auto root = PortableDir(), executable = root + L"\\WindowsStreamTunnel.exe";
    state_ = root + L"\\.windowsstream-tunnel.ini";
    if (GetFileAttributesW(executable.c_str()) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW((root + L"\\wintun.dll").c_str()) == INVALID_FILE_ATTRIBUTES) {
        LogLine("IP 隧道组件缺失，请使用完整控制端文件夹"); return;
    }
    const auto created = proxy::Created(GetCurrentProcess());
    const auto event = L"Local\\WindowsStreamTunnelStop-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(created);
    stop_ = CreateEventW(nullptr, TRUE, FALSE, event.c_str());
    if (!stop_) { LogLine("创建 IP 隧道停止事件失败：%lu", GetLastError()); return; }
    DeleteFileW(state_.c_str());
    const auto arguments = L"--config \"" + ini + L"\" --parent " + std::to_wstring(GetCurrentProcessId()) + L" --created " +
        std::to_wstring(created) + L" --stop \"" + event + L"\"";
    HANDLE token = nullptr; TOKEN_ELEVATION elevation{}; DWORD size = 0;
    const bool elevated = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) &&
        GetTokenInformation(token, TokenElevation, &elevation, sizeof elevation, &size) && elevation.TokenIsElevated;
    if (token) CloseHandle(token);
    if (elevated) {
        auto command = L"\"" + executable + L"\" " + arguments;
        STARTUPINFOW startup{sizeof startup}; startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION child{};
        if (CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, root.c_str(), &startup, &child)) {
            CloseHandle(child.hThread); process_ = child.hProcess;
        }
    } else {
        SHELLEXECUTEINFOW launch{sizeof launch}; launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
        launch.lpVerb = L"runas"; launch.lpFile = executable.c_str(); launch.lpParameters = arguments.c_str();
        launch.lpDirectory = root.c_str(); launch.nShow = SW_HIDE;
        if (ShellExecuteExW(&launch)) process_ = launch.hProcess;
    }
    if (!process_) LogLine("IP 隧道未能启动，需要允许管理员授权：%lu；网页分流仍保留", GetLastError());
    else LogLine("IP 隧道进程已启动，等待真实能力与路由就绪，进程 %lu", GetProcessId(process_));
}
std::wstring IpTunnel::Status() {
    if (!process_) return L"error";
    DWORD exit = 0;
    if (!GetExitCodeProcess(process_, &exit) || exit != STILL_ACTIVE) {
        if (lastError_.empty()) { LogLine("IP 隧道进程已结束，请查看 tunnel.log；不报告为已连接"); lastError_ = L"进程已结束"; }
        return L"error";
    }
    if (GetPrivateProfileIntW(L"tunnel", L"process", 0, state_.c_str()) != GetProcessId(process_)) return L"starting";
    wchar_t status[32]{}; GetPrivateProfileStringW(L"tunnel", L"status", L"starting", status, 32, state_.c_str());
    if (std::wstring(status) == L"error") {
        wchar_t message[2048]{}; GetPrivateProfileStringW(L"tunnel", L"message", L"", message, 2048, state_.c_str());
        if (lastError_ != message) { lastError_ = message; LogLine("IP 隧道未就绪：%s", WideToUtf8(message).c_str()); }
    }
    return status;
}
bool IpTunnel::Ready() { return Status() == L"ready"; }
bool IpTunnel::Starting() { return Status() == L"starting"; }
void IpTunnel::Stop() {
    if (stop_) SetEvent(stop_);
    if (process_) {
        if (WaitForSingleObject(process_, 8000) == WAIT_TIMEOUT) LogLine("IP 隧道仍在清理，停止事件已送达，保留进程自行退出");
        CloseHandle(process_); process_ = nullptr;
    }
    if (stop_) { CloseHandle(stop_); stop_ = nullptr; }
    attempted_ = false; lastError_.clear();
}
