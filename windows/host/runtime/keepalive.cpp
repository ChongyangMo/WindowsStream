#include "keepalive.h"
#include "../../shared/update_package.h"
#include "../../shared/crypto.h"

uint64_t ProcessCreation(HANDLE process) {
    FILETIME created{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exit, &kernel, &user)) return 0;
    return static_cast<uint64_t>(created.dwHighDateTime) << 32 | created.dwLowDateTime;
}
bool KeepAlive::Start() {
    if (child_ && WaitForSingleObject(child_, 0) == WAIT_TIMEOUT) return true;
    if (child_) {
        LogLine("被控端守护进程意外停止，正在恢复守护");
        Stop();
    }
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
    uiBeat_ = CreateEventW(&security, FALSE, FALSE, nullptr);
    workerBeat_ = CreateEventW(&security, FALSE, FALSE, nullptr);
    if (!stop_ || !uiBeat_ || !workerBeat_) { Stop(); return false; }
    HANDLE parent = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &parent,
                         SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, TRUE, 0)) { Stop(); return false; }
    HANDLE inherited[] = {parent, stop_, uiBeat_, workerBeat_};
    std::wstring command = L"\"" + helper.wstring() + L"\" --watchdog " + std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(created);
    for (auto handle : inherited) command += L" " + std::to_wstring(reinterpret_cast<uintptr_t>(handle));
    // 仅传递经验证的原进程句柄和守护事件。
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<uint8_t> attributes(attributeBytes);
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW; startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    PROCESS_INFORMATION child{};
    const bool initialized = InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributeBytes) != FALSE;
    const bool launched = initialized && UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        inherited, sizeof inherited, nullptr, nullptr) && CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, root.c_str(), &startup.StartupInfo, &child);
    if (initialized) DeleteProcThreadAttributeList(startup.lpAttributeList);
    CloseHandle(parent);
    if (!launched) { Stop(); return false; }
    CloseHandle(child.hThread); child_ = child.hProcess;
    nextRetry_ = 0;
    LogLine("被控端守护已启动，进程 %lu；主动退出、升级和注销时停止恢复", child.dwProcessId);
    return true;
}

void KeepAlive::Pulse(bool workerProgress) {
    if (!child_ || WaitForSingleObject(child_, 0) != WAIT_TIMEOUT) {
        const auto now = GetTickCount64();
        if (now < nextRetry_) return;
        if (!Start()) {
            nextRetry_ = now + 30000;
            LogLine("恢复被控端守护失败，30 秒后重试");
            return;
        }
    }
    SetEvent(uiBeat_);
    if (workerProgress) SetEvent(workerBeat_);
}

void KeepAlive::Stop() {
    if (stop_) SetEvent(stop_);
    if (child_) { WaitForSingleObject(child_, 3000); CloseHandle(child_); child_ = nullptr; }
    if (stop_) { CloseHandle(stop_); stop_ = nullptr; }
    if (uiBeat_) { CloseHandle(uiBeat_); uiBeat_ = nullptr; }
    if (workerBeat_) { CloseHandle(workerBeat_); workerBeat_ = nullptr; }
}
