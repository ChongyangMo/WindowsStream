#include "common.h"

#include <shlobj.h>

#include <cstdarg>
#include <cstdio>

namespace { std::wstring runtimeDirectory; }
void SetRuntimeDirectory(const std::wstring& directory) { runtimeDirectory = directory; }

uint64_t NowMicros() {
    static const LARGE_INTEGER frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<uint64_t>(now.QuadPart / frequency.QuadPart * 1000000 +
                                 now.QuadPart % frequency.QuadPart * 1000000 / frequency.QuadPart);
}

std::wstring PortableDir() {
    if (!runtimeDirectory.empty()) return runtimeDirectory;
    wchar_t module[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, module, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring dir(module, n);
    dir.erase(dir.find_last_of(L'\\'));
    return GetFileAttributesW((dir + L"\\client.ini").c_str()) != INVALID_FILE_ATTRIBUTES ||
           GetFileAttributesW((dir + L"\\host.ini").c_str()) != INVALID_FILE_ATTRIBUTES ? dir : std::wstring();
}

static std::wstring LogPath() {
    if (std::wstring portable = PortableDir(); !portable.empty()) {
        return portable + (GetFileAttributesW((portable + L"\\host.ini").c_str()) != INVALID_FILE_ATTRIBUTES
                           ? L"\\host.log" : L"\\client.log");
    }
    PWSTR base = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        path = std::wstring(base) + L"\\WindowsStream";
        CreateDirectoryW(path.c_str(), nullptr);
        path += L"\\client.log";
    }
    CoTaskMemFree(base);
    return path;
}

void LogLine(const char* format, ...) {
    static std::mutex mutex;
    static const std::wstring path = LogPath();
    char message[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof message, format, args);
    va_end(args);

    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[2200];
    int n = snprintf(line, sizeof line, "%04d-%02d-%02d %02d:%02d:%02d.%03d %s\r\n", t.wYear, t.wMonth, t.wDay, t.wHour,
                     t.wMinute, t.wSecond, t.wMilliseconds, message);

    std::lock_guard lock(mutex);
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        if (GetFileSizeEx(file, &size) && size.QuadPart > 4 * 1024 * 1024) {
            CloseHandle(file);
            file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, 0, nullptr);
        }
        DWORD written = 0;
        WriteFile(file, line, static_cast<DWORD>(n), &written, nullptr);
        CloseHandle(file);
    }
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}
