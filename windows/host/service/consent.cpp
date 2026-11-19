#include "service.h"
#include "../input.h"
#include "../session.h"
#include <sddl.h>
#include <thread>
#include <wtsapi32.h>

namespace servermode {
namespace {
bool consentAgent = false;

std::wstring Setting(const wchar_t* key) {
    wchar_t value[256]{};
    const auto ini = PortableDir() + L"\\host.ini";
    GetPrivateProfileStringW(L"consent", key, L"", value, _countof(value), ini.c_str());
    return value;
}

HWND ConsentWindow() {
    if (!ConsentAgent() || !DesktopAccessible()) return nullptr;
    const auto window = GetForegroundWindow();
    if (!window) return nullptr;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return nullptr;
    wchar_t image[MAX_PATH]{}, system[MAX_PATH]{};
    DWORD size = _countof(image);
    const bool found = QueryFullProcessImageNameW(process, 0, image, &size) != FALSE;
    CloseHandle(process);
    const auto length = GetSystemDirectoryW(system, _countof(system));
    if (!found || !length || length >= _countof(system)) return nullptr;
    return _wcsicmp(image, (std::wstring(system) + L"\\consent.exe").c_str()) == 0 ? window : nullptr;
}

HANDLE CreateReadyEvent() {
    const auto device = Setting(L"event_id");
    if (device.size() != 64 || device.find_first_not_of(L"0123456789abcdef") != std::wstring::npos) return nullptr;
    const auto name = L"Global\\WindowsStreamConsent-" + device;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:SYD:P(A;;GA;;;SY)(A;;0x00120000;;;BA)",
            SDDL_REVISION_1, &descriptor, nullptr)) return nullptr;
    SECURITY_ATTRIBUTES attributes{sizeof attributes, descriptor, FALSE};
    const auto event = CreateEventW(&attributes, TRUE, FALSE, name.c_str());
    const auto error = GetLastError();
    LocalFree(descriptor);
    if (event && error == ERROR_ALREADY_EXISTS) { CloseHandle(event); return nullptr; }
    return event;
}
}

bool ConsentAgent() { return consentAgent; }
bool ConsentActive() { return ConsentWindow() != nullptr; }

bool ConsentSession(DWORD session) {
    const auto expected = Setting(L"user_sid");
    if (expected.empty() || session == 0 || session == 0xffffffff) return false;
    HANDLE token = nullptr;
    if (!WTSQueryUserToken(session, &token)) return false;
    alignas(TOKEN_USER) BYTE bytes[256]{};
    DWORD size = 0;
    const bool queried = GetTokenInformation(token, TokenUser, bytes, sizeof bytes, &size) != FALSE;
    CloseHandle(token);
    wchar_t* sid = nullptr;
    if (!queried || !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes)->User.Sid, &sid)) return false;
    const bool matched = expected == sid;
    LocalFree(sid);
    return matched;
}

bool ConsentInput(uint8_t type, const std::vector<uint8_t>& payload) {
    if (type == MsgReleaseAll) return true;
    if (type == MsgMouseMove || type == MsgKey) return ConsentActive();
    if (type != MsgMouseButton || payload.size() != 6 || payload[0] != 0) return false;
    const auto window = ConsentWindow();
    if (!window) return false;
    if (payload[1] == 0) return true;
    RECT bounds{};
    const auto point = PrimaryInputPoint(ReadU16(payload.data() + 2), ReadU16(payload.data() + 4));
    if (!GetWindowRect(window, &bounds) || !PtInRect(&bounds, point)) return false;
    DWORD consentPid = 0, targetPid = 0;
    GetWindowThreadProcessId(window, &consentPid);
    GetWindowThreadProcessId(WindowFromPoint(point), &targetPid);
    return consentPid != 0 && consentPid == targetPid;
}

int RunConsentAgent() {
    DWORD session = 0;
    if (!LocalSystem() || !ProcessIdToSessionId(GetCurrentProcessId(), &session) || !ConsentSession(session)) return 1;
    HostConfig config;
    std::wstring error;
    if (!LoadHostConfig(config, error) || config.reverse || config.bindHost != L"127.0.0.1") return 1;
    config.clipboard = false;
    config.encoder = L"software";
    consentAgent = true;
    const auto event = CreateReadyEvent();
    if (!event) { LogLine("无法创建 UAC 会话状态"); return 2; }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) { CloseHandle(event); return 2; }
    LogLine("UAC 会话代理等待确认窗口");
    for (;;) {
        if (!ConsentActive()) { Sleep(200); continue; }
        HostServer server;
        std::jthread monitor([&](std::stop_token stop) {
            while (!stop.stop_requested() && ConsentActive()) Sleep(100);
            ResetEvent(event);
            server.Stop();
        });
        server.Run(config, nullptr, [&](const std::wstring& state) {
            if (state == L"等待连接" && ConsentActive()) SetEvent(event);
        });
        monitor.request_stop();
        monitor.join();
        ResetEvent(event);
        Sleep(200);
    }
}
}
