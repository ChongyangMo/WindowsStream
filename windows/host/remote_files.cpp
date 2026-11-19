#include "remote_files.h"
#include "../shared/file_drag_source.h"
#include "../shared/remote_files.h"
#include <array>
#include <filesystem>

namespace {
bool ValidAlias(const std::wstring& alias) {
    return !alias.empty() && alias.size() <= 32 &&
        alias.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == std::wstring::npos;
}

bool IsLocalPath(const std::wstring& path) {
    return path.size() > 3 && path[1] == L':' && path[2] == L'\\' &&
        GetDriveTypeW(path.substr(0, 3).c_str()) == DRIVE_FIXED;
}

filedrag::FileHandle Open(const std::wstring& path, DWORD access, DWORD flags) {
    const auto raw = CreateFileW(filedrag::ExtendedPath(path).c_str(), access,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, flags, nullptr);
    return filedrag::FileHandle(raw == INVALID_HANDLE_VALUE ? nullptr : raw);
}

std::wstring FinalPath(HANDLE handle) {
    std::wstring path(32768, L'\0');
    const auto length = GetFinalPathNameByHandleW(handle, path.data(), static_cast<DWORD>(path.size()), FILE_NAME_NORMALIZED);
    if (!length || length >= path.size() || !path.starts_with(L"\\\\?\\")) return {};
    path.resize(length);
    return IsLocalPath(path.substr(4)) ? path : std::wstring{};
}

bool CanReadName(const std::wstring& name) {
    if (!filedrag::ValidName(name)) return false;
    const auto suffix = name.find_last_of(L'.');
    if (suffix == std::wstring::npos) return false;
    const auto extension = name.c_str() + suffix;
    return _wcsicmp(extension, L".log") == 0 || _wcsicmp(extension, L".lvilog") == 0 || _wcsicmp(extension, L".txt") == 0;
}

void AppendText(Writer& output, const std::wstring& text) {
    const auto utf8 = WideToUtf8(text);
    output.append(utf8.data(), utf8.size());
}

DWORD ListDirectory(const std::wstring& directory, Writer& output) {
    WIN32_FIND_DATAW entry{};
    const auto search = FindFirstFileW((directory + L"\\*").c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : error;
    }
    struct FindCloser { void operator()(void* handle) const { FindClose(handle); } };
    std::unique_ptr<void, FindCloser> guard(search);
    std::vector<filedrag::File> entries;
    do {
        if (entry.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT) ||
            !CanReadName(entry.cFileName)) continue;
        entries.push_back({entry.cFileName,
            (static_cast<uint64_t>(entry.nFileSizeHigh) << 32) | entry.nFileSizeLow,
            (static_cast<uint64_t>(entry.ftLastWriteTime.dwHighDateTime) << 32) | entry.ftLastWriteTime.dwLowDateTime});
    } while (FindNextFileW(search, &entry));
    const auto error = GetLastError();
    if (error != ERROR_NO_MORE_FILES) return error;
    const auto count = std::min<size_t>(entries.size(), 200);
    std::partial_sort(entries.begin(), entries.begin() + count, entries.end(), [](const auto& a, const auto& b) {
        return a.modified != b.modified ? a.modified > b.modified : a.name < b.name;
    });
    for (size_t i = 0; i < count; ++i) {
        const auto& file = entries[i];
        const FILETIME modified{static_cast<DWORD>(file.modified), static_cast<DWORD>(file.modified >> 32)};
        SYSTEMTIME utc{};
        FileTimeToSystemTime(&modified, &utc);
        wchar_t date[32]{};
        swprintf_s(date, L"%04u-%02u-%02uT%02u:%02u:%02uZ", utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond);
        AppendText(output, file.name + L"\t" + std::to_wstring(file.size) + L"\t" + date + L"\n");
    }
    return ERROR_SUCCESS;
}
}

void RemoteFilesHost::LoadRoots(const std::wstring& ini) {
    std::array<wchar_t, 16384> values{};
    const auto length = GetPrivateProfileSectionW(L"remote_files", values.data(), static_cast<DWORD>(values.size()), ini.c_str());
    if (length == values.size() - 2) {
        LogLine("远程日志目录配置过长，未启用后台读取");
        return;
    }
    for (const auto* value = values.data(); *value; value += wcslen(value) + 1) {
        const std::wstring entry(value);
        const auto split = entry.find(L'=');
        if (split == std::wstring::npos) continue;
        const auto name = entry.substr(0, split);
        const auto directory = entry.substr(split + 1);
        if (!ValidAlias(name) || !IsLocalPath(directory) || FindRoot(name)) {
            LogLine("忽略无效的远程日志目录配置");
            continue;
        }
        roots_.push_back({name, std::filesystem::path(directory).lexically_normal().wstring()});
    }
}

bool RemoteFilesHost::Start(bool allowed, const std::wstring& ini) {
    if (!allowed) return true;
    LoadRoots(ini);
    return roots_.empty() || channel_.Send(MsgRemoteFilesAvailable, Writer{});
}

const RemoteFilesHost::Root* RemoteFilesHost::FindRoot(const std::wstring& name) const {
    const auto root = std::find_if(roots_.begin(), roots_.end(), [&](const Root& item) {
        return _wcsicmp(item.name.c_str(), name.c_str()) == 0;
    });
    return root == roots_.end() ? nullptr : &*root;
}

DWORD RemoteFilesHost::List(const std::wstring& target, Writer& output) const {
    if (target.empty()) {
        for (const auto& root : roots_) AppendText(output, root.name + L"\n");
        return ERROR_SUCCESS;
    }
    const auto root = FindRoot(target);
    if (!root) return ERROR_ACCESS_DENIED;
    const auto directory = Open(root->directory, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_FLAG_BACKUP_SEMANTICS);
    if (!directory) return GetLastError();
    const auto path = FinalPath(directory.get());
    return path.empty() ? ERROR_ACCESS_DENIED : ListDirectory(path, output);
}

DWORD RemoteFilesHost::Read(const std::wstring& target, bool tail, Writer& output) const {
    const auto separator = target.find(L'/');
    if (separator == std::wstring::npos) return ERROR_INVALID_NAME;
    const auto root = FindRoot(target.substr(0, separator));
    const auto name = target.substr(separator + 1);
    if (!root || !CanReadName(name)) return ERROR_ACCESS_DENIED;
    const auto directory = Open(root->directory, FILE_READ_ATTRIBUTES, FILE_FLAG_BACKUP_SEMANTICS);
    if (!directory) return GetLastError();
    const auto path = FinalPath(directory.get());
    if (path.empty()) return ERROR_ACCESS_DENIED;
    const auto file = Open(path + L"\\" + name, GENERIC_READ, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN);
    if (!file) return GetLastError();
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileType(file.get()) != FILE_TYPE_DISK || !GetFileInformationByHandle(file.get(), &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || info.nNumberOfLinks != 1)
        return ERROR_ACCESS_DENIED;
    const auto final = FinalPath(file.get());
    if (final.size() <= path.size() || final[path.size()] != L'\\' ||
        _wcsnicmp(final.c_str(), path.c_str(), path.size()) != 0 ||
        final.find(L'\\', path.size() + 1) != std::wstring::npos) return ERROR_ACCESS_DENIED;
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file.get(), &length)) return GetLastError();
    if (length.QuadPart > remotefiles::kMaxContent) {
        if (!tail) return ERROR_FILE_TOO_LARGE;
        LARGE_INTEGER offset{};
        offset.QuadPart = length.QuadPart - remotefiles::kMaxContent;
        if (!SetFilePointerEx(file.get(), offset, nullptr, FILE_BEGIN)) return GetLastError();
        length.QuadPart = remotefiles::kMaxContent;
    }
    const auto header = output.bytes.size();
    output.bytes.resize(header + static_cast<size_t>(length.QuadPart));
    DWORD received = 0;
    if (length.QuadPart && !ReadFile(file.get(), output.bytes.data() + header, static_cast<DWORD>(length.QuadPart), &received, nullptr))
        return GetLastError();
    output.bytes.resize(header + received);
    return ERROR_SUCCESS;
}

bool RemoteFilesHost::Handle(uint8_t type, const std::vector<uint8_t>& request) {
    if (type != MsgRemoteFilesRequest) return false;
    if (request.size() < remotefiles::kRequestHeader || request.size() > remotefiles::kRequestHeader + remotefiles::kMaxTarget)
        return true;
    const auto id = ReadU64(request.data());
    auto response = remotefiles::Result(id, ERROR_SUCCESS);
    DWORD error = ERROR_ACCESS_DENIED;
    if (!roots_.empty()) {
        const std::string text(request.begin() + remotefiles::kRequestHeader, request.end());
        const auto target = Utf8ToWide(text);
        const auto operation = static_cast<remotefiles::Operation>(request[8]);
        if (target.find(L'\0') != std::wstring::npos || WideToUtf8(target) != text) {
            error = ERROR_INVALID_NAME;
        } else if (operation == remotefiles::Operation::List) {
            error = List(target, response);
        } else if (operation == remotefiles::Operation::Read || operation == remotefiles::Operation::Tail) {
            error = Read(target, operation == remotefiles::Operation::Tail, response);
        } else {
            error = ERROR_INVALID_FUNCTION;
        }
        LogLine("后台日志访问：操作=%u，结果=%lu", static_cast<unsigned>(operation), error);
    }
    if (error) response = remotefiles::Result(id, error);
    if (!channel_.Send(MsgRemoteFilesResult, response)) channel_.Shutdown();
    return true;
}
