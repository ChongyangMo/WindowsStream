#include "relay.h"
#include "workspace/catalog.h"
#include <charconv>
#include <filesystem>
#include <memory>

namespace {
struct CloseHandleOnExit {
    void operator()(HANDLE handle) const { CloseHandle(handle); }
};
using Handle = std::unique_ptr<void, CloseHandleOnExit>;

uint16_t ReadPort(HANDLE output, HANDLE process) {
    std::string text;
    const auto deadline = GetTickCount64() + 4000;
    while (GetTickCount64() < deadline && text.size() < 16) {
        DWORD available = 0;
        if (!PeekNamedPipe(output, nullptr, 0, nullptr, &available, nullptr)) break;
        if (!available) {
            if (WaitForSingleObject(process, 25) != WAIT_TIMEOUT) break;
            continue;
        }
        char bytes[16]{};
        DWORD size = 0;
        if (!ReadFile(output, bytes, std::min<DWORD>(available, sizeof bytes), &size, nullptr) || !size) break;
        text.append(bytes, size);
        auto newline = text.find('\n');
        if (newline == std::string::npos) continue;
        if (newline && text[newline - 1] == '\r') --newline;
        unsigned port = 0;
        const auto end = text.data() + newline;
        const auto parsed = std::from_chars(text.data(), end, port);
        if (parsed.ec == std::errc{} && parsed.ptr == end && port && port <= 65535) {
            return static_cast<uint16_t>(port);
        }
        break;
    }
    return 0;
}

bool StartBridge(const std::filesystem::path& executable, const std::wstring& config,
                 HANDLE job, uint16_t& port) {
    SECURITY_ATTRIBUTES inherited{sizeof inherited, nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &inherited, 0)) return false;
    Handle output(read), childOutput(write);
    if (!SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0)) return false;
    auto nullDevice = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &inherited, OPEN_EXISTING, 0, nullptr);
    if (nullDevice == INVALID_HANDLE_VALUE) return false;
    Handle nullHandle(nullDevice);

    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<uint8_t> storage(bytes);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nullDevice;
    startup.StartupInfo.hStdOutput = write;
    startup.StartupInfo.hStdError = nullDevice;
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes)) return false;
    HANDLE handles[]{write, nullDevice};
    PROCESS_INFORMATION child{};
    auto command = L"\"" + executable.wstring() + L"\" --viewer-config \"" + config + L"\"";
    const auto directory = executable.parent_path();
    const bool created = UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        handles, sizeof handles, nullptr, nullptr) &&
        CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, directory.c_str(),
            &startup.StartupInfo, &child);
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (!created) return false;
    Handle process(child.hProcess), thread(child.hThread);
    childOutput.reset();
    if (!AssignProcessToJobObject(job, process.get()) || ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
        TerminateProcess(process.get(), 1);
        return false;
    }
    port = ReadPort(output.get(), process.get());
    return port != 0;
}
}

std::wstring ViewerRelayConfig(const std::wstring& profilePath) {
    const auto path = (std::filesystem::path(profilePath).parent_path() / L"relay.ini").wstring();
    return workspace::ReadSetting(path, L"relay", L"role") == L"viewer" ? path : L"";
}

ViewerRelay::~ViewerRelay() {
    if (job_) CloseHandle(job_);
}

bool ViewerRelay::Start(const std::wstring& profilePath, uint16_t& port, std::wstring& error) {
    const auto config = ViewerRelayConfig(profilePath);
    if (config.empty()) return true;
    const auto executable = std::filesystem::path(workspace::ExecutablePath()).parent_path() /
        L".runtime" / L"WindowsStreamBridge.exe";
    if (GetFileAttributesW(executable.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = L"缺少中转组件，请使用完整的 WindowsStream 控制端";
        return false;
    }
    Handle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof limits) ||
        !StartBridge(executable, config, job.get(), port)) {
        error = L"中转进程未能启动，请查看中转日志";
        return false;
    }
    job_ = job.release();
    return true;
}
