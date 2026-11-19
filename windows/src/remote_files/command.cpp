#include "session.h"
#include "../runtime/workspace/catalog.h"
#include "../../shared/remote_files.h"
#include <filesystem>
#include <shellapi.h>

namespace remotefiles {
namespace {
int Report(DWORD code) {
    if (!code) return 0;
    std::wstring message;
    switch (code) {
        case ERROR_FILE_NOT_FOUND: message = L"找不到当前会话的后台接口或指定文件"; break;
        case ERROR_NOT_CONNECTED: message = L"该 WindowsStream 会话尚未连接"; break;
        case ERROR_NOT_SUPPORTED: message = L"被控端版本不支持后台读取，或未配置日志目录"; break;
        case ERROR_FILE_TOO_LARGE: message = L"文件超过 4 MiB，请改用 tail 读取末尾"; break;
        case ERROR_INVALID_PARAMETER: message = L"用法：--remote-files <client.ini> <list|read|tail> <目录别名|-|别名/文件名> <本地输出>"; break;
        default: message = L"后台读取失败"; break;
    }
    message += L"（错误码 " + std::to_wstring(code) + L"）\r\n";
    auto output = GetStdHandle(STD_ERROR_HANDLE);
    if (!output || output == INVALID_HANDLE_VALUE) {
        AttachConsole(ATTACH_PARENT_PROCESS);
        output = GetStdHandle(STD_ERROR_HANDLE);
    }
    if (output && output != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        DWORD mode = 0;
        if (GetConsoleMode(output, &mode)) WriteConsoleW(output, message.data(), static_cast<DWORD>(message.size()), &written, nullptr);
        else {
            const auto utf8 = WideToUtf8(message);
            WriteFile(output, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        }
    }
    return static_cast<int>(code);
}

DWORD Save(const std::wstring& destination, const uint8_t* data, DWORD size) {
    const auto temporary = destination + L".tmp-" + std::to_wstring(GetCurrentProcessId());
    const auto raw = CreateFileW(filedrag::ExtendedPath(temporary).c_str(), GENERIC_WRITE, 0,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (raw == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD written = 0;
    DWORD error = ERROR_SUCCESS;
    if (size && !WriteFile(raw, data, size, &written, nullptr)) error = GetLastError();
    else if (written != size) error = ERROR_WRITE_FAULT;
    if (!CloseHandle(raw) && !error) error = GetLastError();
    if (!error && !MoveFileExW(filedrag::ExtendedPath(temporary).c_str(), filedrag::ExtendedPath(destination).c_str(),
                               MOVEFILE_WRITE_THROUGH)) error = GetLastError();
    if (error) DeleteFileW(filedrag::ExtendedPath(temporary).c_str());
    return error;
}

DWORD Run(const wchar_t* const* args) {
    if (!*args[2] || !*args[5]) return ERROR_INVALID_PARAMETER;
    const auto profile = workspace::CanonicalPath(args[2]);
    const std::wstring action = args[3];
    const std::wstring target = args[4];
    std::error_code pathError;
    const auto output = std::filesystem::absolute(args[5], pathError).lexically_normal().wstring();
    if (pathError) return ERROR_INVALID_NAME;
    Operation operation;
    if (action == L"list") operation = Operation::List;
    else if (action == L"read") operation = Operation::Read;
    else if (action == L"tail") operation = Operation::Tail;
    else return ERROR_INVALID_PARAMETER;
    if (profile.empty() || output.empty() || target.empty() || (target == L"-" && operation != Operation::List))
        return ERROR_INVALID_PARAMETER;
    const auto name = target == L"-" ? std::string{} : WideToUtf8(target);
    if (name.size() > kMaxTarget || (target != L"-" && name.empty())) return ERROR_INVALID_PARAMETER;
    DWORD error = ERROR_SUCCESS;
    const auto pipe = OpenServerPipe(PipeName(profile), error);
    if (!pipe) return error;
    Writer request;
    request.u8(static_cast<uint8_t>(operation));
    request.append(name.data(), name.size());
    DWORD transferred = 0;
    if (!Transfer(pipe.get(), nullptr, true, request.bytes.data(), static_cast<DWORD>(request.bytes.size()), transferred, 2000))
        return GetLastError();
    if (transferred != request.bytes.size()) return ERROR_WRITE_FAULT;
    std::vector<uint8_t> response(kMaxContent + kResultHeader);
    if (!Transfer(pipe.get(), nullptr, false, response.data(), static_cast<DWORD>(response.size()), transferred, 15000))
        return GetLastError();
    if (transferred < kResultHeader) return ERROR_INVALID_DATA;
    uint8_t acknowledged = 1;
    DWORD ackBytes = 0;
    Transfer(pipe.get(), nullptr, true, &acknowledged, 1, ackBytes, 1000);
    error = ReadU32(response.data() + 8);
    if (error) return error;
    return Save(output, response.data() + kResultHeader, transferred - static_cast<DWORD>(kResultHeader));
}
}

int DispatchCommand() {
    int count = 0;
    auto args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args) return -1;
    const bool match = count >= 2 && wcscmp(args[1], L"--remote-files") == 0;
    const auto code = match ? (count == 6 ? Run(args) : ERROR_INVALID_PARAMETER) : ERROR_SUCCESS;
    LocalFree(args);
    return match ? Report(code) : -1;
}
}
