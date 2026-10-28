#include "service.h"
#include <sddl.h>
#include <wtsapi32.h>

namespace servermode {
HANDLE CreateAttentionEvent(std::wstring& name) {
    GUID id{}; wchar_t value[40]{};
    if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, value, 40)) return nullptr;
    name = L"Global\\WindowsStreamServerSAS-" + std::wstring(value);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1, &descriptor, nullptr)) return nullptr;
    SECURITY_ATTRIBUTES security{sizeof security, descriptor, FALSE};
    HANDLE event = CreateEventW(&security, FALSE, FALSE, name.c_str());
    LocalFree(descriptor);
    return event;
}
void SendAttention(HANDLE agent) {
    DWORD process = agent ? GetProcessId(agent) : 0, session = 0;
    if (!process || !ProcessIdToSessionId(process, &session)) return;
    // 控制台的安全注意序列由真正的系统服务发起，无需模拟用户身份。
    if (session == WTSGetActiveConsoleSessionId()) {
        const auto library = LoadLibraryExW(L"sas.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        const auto send = library ? reinterpret_cast<void(WINAPI*)(BOOL)>(GetProcAddress(library, "SendSAS")) : nullptr;
        if (send) { send(FALSE); LogLine("系统服务已向控制台会话发送安全注意序列：%lu", session); }
        else LogLine("安全注意序列接口不可用：%lu", GetLastError());
        if (library) FreeLibrary(library);
        return;
    }
    HANDLE source = nullptr, token = nullptr;
    // 远程桌面会话使用该会话已登录用户的令牌定位 SAS；系统令牌可能仍指向控制台。
    if (!WTSQueryUserToken(session, &source) && !OpenProcessToken(agent, TOKEN_DUPLICATE | TOKEN_QUERY, &source)) return;
    const bool copied = DuplicateTokenEx(source, TOKEN_QUERY | TOKEN_IMPERSONATE, nullptr, SecurityImpersonation, TokenImpersonation, &token) != FALSE;
    CloseHandle(source);
    if (!copied) return;
    const auto library = LoadLibraryExW(L"sas.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto send = library ? reinterpret_cast<void(WINAPI*)(BOOL)>(GetProcAddress(library, "SendSAS")) : nullptr;
    if (send && ImpersonateLoggedOnUser(token)) {
        send(FALSE);
        RevertToSelf();
        LogLine("系统服务已向当前远程会话发送安全注意序列");
    } else LogLine("系统服务发送安全注意序列失败：%lu", GetLastError());
    if (library) FreeLibrary(library);
    CloseHandle(token);
}
}
