#include "pipe.h"
#include <array>
#include <cwctype>
#include <sddl.h>
#include <aclapi.h>

namespace remotefiles {
namespace {
std::wstring UserSid() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return {};
    filedrag::FileHandle token(raw);
    alignas(TOKEN_USER) std::array<BYTE, sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE> buffer{};
    DWORD size = 0;
    if (!GetTokenInformation(token.get(), TokenUser, buffer.data(), static_cast<DWORD>(buffer.size()), &size)) return {};
    wchar_t* text = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text)) return {};
    const std::wstring sid(text);
    LocalFree(text);
    return sid;
}

bool Complete(HANDLE pipe, HANDLE stop, OVERLAPPED& operation, DWORD timeout, DWORD& transferred) {
    const HANDLE events[] = {operation.hEvent, stop};
    if (WaitForMultipleObjects(stop ? 2 : 1, events, FALSE, timeout) == WAIT_OBJECT_0)
        return GetOverlappedResult(pipe, &operation, &transferred, FALSE) != FALSE;
    CancelIoEx(pipe, &operation);
    GetOverlappedResult(pipe, &operation, &transferred, TRUE);
    SetLastError(ERROR_TIMEOUT);
    return false;
}
}

std::wstring PipeName(const std::wstring& profile) {
    const auto sid = UserSid();
    DWORD session = 0;
    if (sid.empty() || !ProcessIdToSessionId(GetCurrentProcessId(), &session)) return {};
    uint64_t hash = 14695981039346656037ULL;
    for (const auto c : profile) {
        hash ^= static_cast<uint64_t>(towlower(c));
        hash *= 1099511628211ULL;
    }
    return L"\\\\.\\pipe\\WindowsStreamFiles-" + sid + L"-" + std::to_wstring(session) + L"-" + std::to_wstring(hash);
}

filedrag::FileHandle CreateServerPipe(const std::wstring& name) {
    const auto sid = UserSid();
    if (name.empty() || sid.empty()) return {};
    const auto sddl = L"O:" + sid + L"D:P(A;;GA;;;" + sid + L")";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return {};
    SECURITY_ATTRIBUTES attributes{sizeof attributes, descriptor, FALSE};
    const auto pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 4096, 0, &attributes);
    LocalFree(descriptor);
    return filedrag::FileHandle(pipe == INVALID_HANDLE_VALUE ? nullptr : pipe);
}

filedrag::FileHandle OpenServerPipe(const std::wstring& name, DWORD& error) {
    error = ERROR_FILE_NOT_FOUND;
    if (name.empty() || !WaitNamedPipeW(name.c_str(), 2000)) {
        if (!name.empty()) error = GetLastError();
        return {};
    }
    const auto raw = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return {};
    }
    filedrag::FileHandle pipe(raw);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PSID owner = nullptr;
    const auto status = GetSecurityInfo(raw, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &descriptor);
    wchar_t* text = nullptr;
    const bool own = status == ERROR_SUCCESS && owner && ConvertSidToStringSidW(owner, &text) && UserSid() == text;
    if (text) LocalFree(text);
    if (descriptor) LocalFree(descriptor);
    if (!own) {
        error = ERROR_ACCESS_DENIED;
        return {};
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(raw, &mode, nullptr, nullptr)) {
        error = GetLastError();
        return {};
    }
    error = ERROR_SUCCESS;
    return pipe;
}

bool AcceptClient(HANDLE pipe, HANDLE stop) {
    filedrag::FileHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) return false;
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    if (ConnectNamedPipe(pipe, &operation)) return true;
    const auto error = GetLastError();
    DWORD transferred = 0;
    return error == ERROR_PIPE_CONNECTED ||
        (error == ERROR_IO_PENDING && Complete(pipe, stop, operation, INFINITE, transferred));
}

bool Transfer(HANDLE pipe, HANDLE stop, bool write, void* buffer, DWORD size, DWORD& transferred, DWORD timeout) {
    filedrag::FileHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) return false;
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    const bool done = write ? WriteFile(pipe, buffer, size, &transferred, &operation) != FALSE
                            : ReadFile(pipe, buffer, size, &transferred, &operation) != FALSE;
    if (done) return true;
    const bool pending = GetLastError() == ERROR_IO_PENDING;
    if (pending && Complete(pipe, stop, operation, timeout, transferred)) return true;
    const auto error = GetLastError();
    event.reset();
    SetLastError(error);
    return false;
}
}
