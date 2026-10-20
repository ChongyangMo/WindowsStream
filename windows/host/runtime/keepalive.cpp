#include "keepalive.h"
#include "../../shared/update_package.h"
#include "../../shared/crypto.h"

uint64_t ProcessCreation(HANDLE process) {
    FILETIME created{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exit, &kernel, &user)) return 0;
    return static_cast<uint64_t>(created.dwHighDateTime) << 32 | created.dwLowDateTime;
}
bool KeepAlive::Start() {
    if (stop_ || child_) return true;
    const auto module = updates::ModulePath();
    updates::Package package;
    if (!updates::LoadPackage(module, package)) return false;
    const auto hash = updates::Hex(package.hash.data(), package.hash.size());
    const auto root = std::filesystem::path(module).parent_path();
    const auto folder = root / L".windowsstream-runtime" / Utf8ToWide(hash);
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error) return false;
    const auto helper = folder / L"WindowsStreamKeepAlive.exe";
    updates::Package existing;
    if (!updates::LoadPackage(helper, existing) || !SecureEqual(existing.hash.data(), package.hash.data(), 32)) {
        if (!CopyFileW(module.c_str(), helper.c_str(), FALSE)) return false;
    }
    const auto created = ProcessCreation(GetCurrentProcess());
    if (!created) return false;
    SECURITY_ATTRIBUTES security{sizeof security, nullptr, TRUE};
    stop_ = CreateEventW(&security, TRUE, FALSE, nullptr);
    if (!stop_) return false;
    HANDLE parent = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &parent,
                         SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE, 0)) { Stop(); return false; }
    std::wstring command = L"\"" + helper.wstring() + L"\" --watchdog " + std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(created) +
        L" " + std::to_wstring(reinterpret_cast<uintptr_t>(parent)) + L" " + std::to_wstring(reinterpret_cast<uintptr_t>(stop_));
    // 仅传递停止事件与原进程句柄，启动阶段立刻崩溃也不会丢失恢复依据。
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<uint8_t> attributes(attributeBytes);
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW; startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    HANDLE inherited[] = {parent, stop_};
    PROCESS_INFORMATION child{};
    const bool initialized = InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributeBytes) != FALSE;
    const bool launched = initialized && UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        inherited, sizeof inherited, nullptr, nullptr) && CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, root.c_str(), &startup.StartupInfo, &child);
    if (initialized) DeleteProcThreadAttributeList(startup.lpAttributeList);
    CloseHandle(parent);
    if (!launched) { Stop(); return false; }
    CloseHandle(child.hThread); child_ = child.hProcess;
    LogLine("异常退出守护已启动，守护进程 %lu；主动退出、升级和注销时停止恢复", child.dwProcessId);
    return true;
}
void KeepAlive::Stop() {
    if (stop_) SetEvent(stop_);
    if (child_) { WaitForSingleObject(child_, 3000); CloseHandle(child_); child_ = nullptr; }
    if (stop_) { CloseHandle(stop_); stop_ = nullptr; }
}
