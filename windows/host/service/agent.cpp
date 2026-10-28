#include "service.h"
#include "../session.h"
#include "../../shared/host_control.h"
#include <shellapi.h>

namespace servermode {
namespace { bool secureAgent = false; }
bool SecureAgent() { return secureAgent; }
bool LocalSystem() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    BYTE buffer[512]{}; DWORD size = 0;
    const bool result = GetTokenInformation(token, TokenUser, buffer, sizeof buffer, &size) &&
        IsWellKnownSid(reinterpret_cast<TOKEN_USER*>(buffer)->User.Sid, WinLocalSystemSid);
    CloseHandle(token);
    return result;
}
int RunAgent() {
    DWORD session = 0; ProcessIdToSessionId(GetCurrentProcessId(), &session);
    if (!LocalSystem() || !session) { LogLine("服务器会话代理必须由系统服务在交互会话内启动"); return 1; }
    const auto ini = PortableDir() + L"\\host.ini";
    if (!GetPrivateProfileIntW(L"server", L"secure_desktop", 0, ini.c_str())) return 1;
    HostConfig config; std::wstring error;
    if (!LoadHostConfig(config, error) || config.reverse || config.bindHost != L"127.0.0.1") {
        LogLine("服务器会话代理配置无效，仅允许带认证的本机监听模式"); return 1;
    }
    secureAgent = true;
    config.capture = L"gdi"; config.encoder = L"software"; config.clipboard = false;
    config.fps = std::min(config.fps, 30);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    DesktopBinding desktop;
    if (!desktop.Attach()) { LogLine("服务器输入桌面暂不可访问：%lu", GetLastError()); return 2; }
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) return 1;
    LogLine("服务器会话代理已启动，会话 %lu，端口 %u", session, config.port);
    HostServer server;
    server.Run(config, nullptr, [](const std::wstring& state) { LogLine("服务器连接状态：%s", WideToUtf8(state).c_str()); });
    WSACleanup();
    return 0;
}
int Dispatch() {
    int count = 0; auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    const std::wstring command = arguments && count == 2 ? arguments[1] : L"";
    if (arguments) LocalFree(arguments);
    if (command == L"--server-service") return RunService();
    if (command == L"--server-agent") return RunAgent();
    if (command == L"--stop-user-host") {
        const auto window = FindWindowW(L"WindowsStreamHostWindow", nullptr);
        return window && !PostMessageW(window, kHostExitForUpdate, 0, 0) ? 1 : 0;
    }
    return -1;
}
}
