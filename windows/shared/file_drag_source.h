#pragma once

#include "file_drag.h"
#include <memory>
#include <ole2.h>
#include <shellapi.h>

namespace filedrag {
inline FORMATETC DropPathFormat() { return {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}; }

inline std::vector<std::wstring> PathsFromDrop(IDataObject* data) {
    auto format = DropPathFormat();
    STGMEDIUM medium{};
    if (FAILED(data->GetData(&format, &medium))) return {};
    std::vector<std::wstring> paths;
    if (medium.tymed == TYMED_HGLOBAL && medium.hGlobal) {
        const auto drop = reinterpret_cast<HDROP>(medium.hGlobal);
        const UINT count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
        if (count <= kMaxFiles) {
            for (UINT i = 0; i < count; ++i) {
                const UINT length = DragQueryFileW(drop, i, nullptr, 0);
                if (!length || length >= 32767) break;
                std::wstring path(length + 1, L'\0');
                if (DragQueryFileW(drop, i, path.data(), length + 1) != length) break;
                path.resize(length);
                paths.push_back(std::move(path));
            }
        }
        if (paths.size() != count) paths.clear();
    }
    ReleaseStgMedium(&medium);
    return paths;
}

inline std::wstring ExtendedPath(const std::wstring& path) {
    if (path.starts_with(L"\\\\?\\") || path.starts_with(L"\\\\.\\")) return path;
    if (path.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + path.substr(2);
    if (path.size() >= 3 && path[1] == L':' && path[2] == L'\\') return L"\\\\?\\" + path;
    return path;
}

struct CloseFile { void operator()(void* value) const { CloseHandle(value); } };
using FileHandle = std::unique_ptr<void, CloseFile>;

struct Source {
    File file;
    FileHandle handle;

    bool Read(uint64_t offset, uint8_t* data, DWORD length) const {
        if (offset > file.size || length > file.size - offset) return false;
        if (!length) return true;
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        DWORD received = 0;
        return SetFilePointerEx(handle.get(), position, nullptr, FILE_BEGIN) &&
            ReadFile(handle.get(), data, length, &received, nullptr) && received == length;
    }
};

inline std::vector<Source> OpenFiles(const std::vector<std::wstring>& paths) {
    if (paths.size() > kMaxFiles) return {};
    std::vector<Source> files;
    files.reserve(paths.size());
    for (const auto& path : paths) {
        const auto slash = path.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return {};
        auto name = path.substr(slash + 1);
        if (!ValidName(name)) return {};
        const auto raw = CreateFileW(ExtendedPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
        if (raw == INVALID_HANDLE_VALUE) return {};
        Source source;
        source.handle.reset(raw);
        BY_HANDLE_FILE_INFORMATION info{};
        if (GetFileType(raw) != FILE_TYPE_DISK || !GetFileInformationByHandle(raw, &info) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || info.nFileSizeHigh > INT32_MAX) return {};
        source.file = {std::move(name),
            (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow,
            (static_cast<uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime};
        files.push_back(std::move(source));
    }
    return files;
}
}
