#include "system_proxy.h"
#include "split_rules.h"
#include <shellapi.h>

namespace proxy {
namespace {
std::wstring StatePath() { return PortableDir() + L"\\.windowsstream-proxy.ini"; }
std::wstring ModulePath() {
    std::wstring path(32768, L'\0');
    auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length); return path;
}
}
bool SystemProxy::Guard() {
    SECURITY_ATTRIBUTES security{sizeof security, nullptr, TRUE};
    stop_ = CreateEventW(&security, TRUE, FALSE, nullptr);
    if (!stop_) return false;
    HANDLE ready = CreateEventW(&security, TRUE, FALSE, nullptr);
    if (!ready) return false;
    HANDLE parent = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &parent,
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE, 0)) { CloseHandle(ready); return false; }
    const auto module = ModulePath();
    auto command = L"\"" + module + L"\" --proxy-guard " + std::to_wstring(GetCurrentProcessId()) + L" " +
        std::to_wstring(Created(GetCurrentProcess())) + L" " + std::to_wstring(reinterpret_cast<uintptr_t>(parent)) + L" " +
        std::to_wstring(reinterpret_cast<uintptr_t>(stop_)) + L" " + std::to_wstring(reinterpret_cast<uintptr_t>(ready));
    SIZE_T size = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<uint8_t> storage(size);
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW; startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    bool initialized = InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size) != FALSE;
    HANDLE handles[]{parent, stop_, ready}; PROCESS_INFORMATION child{};
    bool launched = initialized && UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        handles, sizeof handles, nullptr, nullptr) && CreateProcessW(module.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, PortableDir().c_str(), &startup.StartupInfo, &child);
    if (initialized) DeleteProcThreadAttributeList(startup.lpAttributeList);
    CloseHandle(parent);
    if (!launched) { CloseHandle(ready); return false; }
    CloseHandle(child.hThread); guard_ = child.hProcess;
    HANDLE waited[]{ready, guard_};
    const bool initializedGuard = WaitForMultipleObjects(2, waited, FALSE, 5000) == WAIT_OBJECT_0;
    CloseHandle(ready);
    return initializedGuard;
}
void SystemProxy::StopGuard(bool restored) {
    if (restored && stop_) SetEvent(stop_);
    if (guard_) { if (restored) WaitForSingleObject(guard_, 3000); CloseHandle(guard_); guard_ = nullptr; }
    if (stop_) { CloseHandle(stop_); stop_ = nullptr; }
}
bool SystemProxy::Start(uint16_t port) {
    if (active_) return true;
    if (!port || PortableDir().empty() || !RestoreSnapshot(StatePath())) return false;
    SettingsLock lock;
    if (!lock.locked) return false;
    ProxySnapshot state; state.process = GetCurrentProcessId(); state.created = Created(GetCurrentProcess());
    if (!state.created || !ReadSystemSettings(state.original)) return false;
    std::string script;
    if (!BuildSplitPac(PortableDir() + L"\\client.ini", port, state.original, script) || !pac_.Start(script)) return false;
    state.applied = state.original;
    state.applied.flags = 1 | 4;
    state.applied.script = pac_.Url();
    if (!SaveSnapshot(StatePath(), state)) { pac_.Stop(); return false; }
    if (!Guard()) { LogLine("启动系统代理恢复守护失败，未启用内网访问"); StopGuard(true); pac_.Stop(); DeleteFileW(StatePath().c_str()); return false; }
    applied_ = state.applied;
    active_ = true;
    if (!WriteSystemSettings(applied_)) { LogLine("启用系统代理失败，正在恢复原设置"); Stop(); return false; }
    LogLine("内网分流已启用，匹配网段和域名经被控端，其余沿用原网络出口"); return true;
}
bool SystemProxy::Owns() const {
    SystemSettings current;
    return active_ && ReadSystemSettings(current) && current == applied_;
}
bool SystemProxy::Stop() {
    if (!active_) return true;
    bool restored = RestoreSnapshot(StatePath(), GetCurrentProcessId(), Created(GetCurrentProcess()));
    if (restored) { active_ = false; StopGuard(true); pac_.Stop(); }
    return restored;
}
int RunProxyGuard() {
    int count = 0; auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments || count < 2 || std::wstring(arguments[1]) != L"--proxy-guard") { if (arguments) LocalFree(arguments); return -1; }
    if (count != 7) { LocalFree(arguments); return 2; }
    DWORD id = wcstoul(arguments[2], nullptr, 10);
    uint64_t created = _wcstoui64(arguments[3], nullptr, 10);
    HANDLE parent = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_wcstoui64(arguments[4], nullptr, 10)));
    HANDLE stop = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_wcstoui64(arguments[5], nullptr, 10)));
    HANDLE ready = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_wcstoui64(arguments[6], nullptr, 10)));
    LocalFree(arguments);
    struct Handles { HANDLE parent, stop, ready; ~Handles() { if (parent) CloseHandle(parent); if (stop) CloseHandle(stop); if (ready) CloseHandle(ready); } } handles{parent, stop, ready};
    if (!parent || !stop || !ready || GetProcessId(parent) != id || !created || Created(parent) != created) return 2;
    std::wstring path(32768, L'\0'); DWORD length = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(parent, 0, path.data(), &length)) return 2;
    path.resize(length);
    if (_wcsicmp(path.c_str(), ModulePath().c_str()) != 0 || PortableDir().empty()) return 2;
    if (!SetEvent(ready)) return 2;
    HANDLE waited[]{stop, parent};
    if (WaitForMultipleObjects(2, waited, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) return 0;
    LogLine("控制端已结束，检查并恢复原系统代理设置");
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (RestoreSnapshot(StatePath(), id, created)) return 0;
        Sleep(1000);
    }
    return 1;
}
}
