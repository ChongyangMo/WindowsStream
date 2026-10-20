#include "apply.h"
#include "../../shared/crypto.h"
#include <shellapi.h>

namespace updates {
bool Stage(const std::filesystem::path& root, DWORD parent, const Package& package,
           const std::vector<uint8_t>& manifest, const std::string& token) {
    uint64_t bytes = 0; uint32_t stamp = 0;
    std::array<uint8_t, 32> expected{};
    if (!VerifyManifest(manifest, token, bytes, stamp, expected) || bytes != package.bytes.size() ||
        stamp != package.stamp || !SecureEqual(expected.data(), package.hash.data(), 32)) return false;
    const auto id = Nonce();
    if (id.empty()) return false;
    const auto folder = root / L".windowsstream-updates" / Utf8ToWide(id);
    std::error_code error;
    if (!std::filesystem::create_directories(folder, error) || error) return false;
    const auto payload = folder / L"payload.exe";
    HANDLE file = CreateFileW(payload.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool saved = WriteFile(file, package.bytes.data(), static_cast<DWORD>(package.bytes.size()), &written, nullptr) &&
                       written == package.bytes.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!saved) return false;
    const auto helper = folder / L"WindowsStreamUpdater.exe";
    if (!CopyFileW(payload.c_str(), helper.c_str(), TRUE)) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent);
    if (!process) return false;
    FILETIME creation{}, exit{}, kernel{}, user{};
    const bool timed = GetProcessTimes(process, &creation, &exit, &kernel, &user) != FALSE;
    CloseHandle(process);
    if (!timed) return false;
    const uint64_t created = static_cast<uint64_t>(creation.dwHighDateTime) << 32 | creation.dwLowDateTime;
    const auto ticket = folder / L"ticket.ini";
    if (!WritePrivateProfileStringW(L"update", L"parent", std::to_wstring(parent).c_str(), ticket.c_str()) ||
        !WritePrivateProfileStringW(L"update", L"created", std::to_wstring(created).c_str(), ticket.c_str()) ||
        !WritePrivateProfileStringW(L"update", L"manifest", Utf8ToWide(Hex(manifest.data(), manifest.size())).c_str(), ticket.c_str())) return false;
    std::wstring command = L"\"" + helper.wstring() + L"\" --apply-update \"" + ticket.wstring() + L"\"";
    STARTUPINFOW startup{sizeof startup};
    startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, folder.c_str(), &startup, &child)) return false;
    CloseHandle(child.hProcess); CloseHandle(child.hThread);
    return true;
}

bool BootstrapExisting() {
    HWND window = FindWindowW(L"WindowsStreamHostWindow", nullptr);
    if (!window) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    std::wstring target(32768, L'\0');
    DWORD length = static_cast<DWORD>(target.size());
    const bool found = QueryFullProcessImageNameW(process, 0, target.data(), &length) != FALSE;
    CloseHandle(process);
    if (!found) return false;
    target.resize(length);
    const auto root = std::filesystem::path(target).parent_path();
    if (_wcsicmp(std::filesystem::path(target).filename().c_str(), L"WindowsStreamHost.exe") != 0) return false;
    wchar_t token[1024]{};
    GetPrivateProfileStringW(L"connection", L"token", L"", token, 1024, (root / L"host.ini").c_str());
    const auto key = WideToUtf8(token);
    if (key.size() < 32) return false;
    Package next, previous;
    if (!LoadPackage(ModulePath(), next) || !LoadPackage(target, previous) || next.stamp <= previous.stamp) return false;
    return Stage(root, pid, next, Manifest(next, key), key);
}

void MarkHealthy(const std::string& bootId) {
    if (bootId.size() != 32 || bootId.find_first_not_of("0123456789abcdef") != std::string::npos) return;
    const auto folder = std::filesystem::path(ModulePath()).parent_path() / L".windowsstream-updates" / Utf8ToWide(bootId);
    Package current;
    if (!LoadPackage(ModulePath(), current)) return;
    WritePrivateProfileStringW(L"health", L"sha256", Utf8ToWide(Hex(current.hash.data(), 32)).c_str(), (folder / L"healthy.ini").c_str());
    WritePrivateProfileStringW(L"health", L"pid", std::to_wstring(GetCurrentProcessId()).c_str(), (folder / L"healthy.ini").c_str());
}
}
