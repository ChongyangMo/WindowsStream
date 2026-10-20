#include "apply.h"
#include "../../shared/crypto.h"
#include "../../shared/host_control.h"
#include "../runtime/awake.h"

namespace updates {
namespace {
void State(const std::filesystem::path& root, const wchar_t* result, const std::string& hash, bool rejected = false) {
    const auto path = root / L"update-state.ini";
    WritePrivateProfileStringW(L"update", L"result", result, path.c_str());
    WritePrivateProfileStringW(L"update", L"sha256", Utf8ToWide(hash).c_str(), path.c_str());
    if (rejected) WritePrivateProfileStringW(L"update", L"rejected_sha256", Utf8ToWide(hash).c_str(), path.c_str());
}
bool Launch(const std::filesystem::path& target, const std::wstring& arguments, PROCESS_INFORMATION& process) {
    std::wstring command = L"\"" + target.wstring() + L"\"" + arguments;
    STARTUPINFOW startup{sizeof startup};
    startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_SHOWMINNOACTIVE;
    return CreateProcessW(target.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                          target.parent_path().c_str(), &startup, &process) != FALSE;
}
BOOL CALLBACK CloseWindow(HWND window, LPARAM pid) {
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner == static_cast<DWORD>(pid)) {
        PostMessageW(window, kHostExitForUpdate, 0, 0);
        PostMessageW(window, WM_CLOSE, 0, 0);  // 兼容尚未加入托盘的旧版本。
    }
    return TRUE;
}
}

int Apply(const std::wstring& ticketText) {
    struct UpgradePower {
        UpgradePower() { SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED); }
        ~UpgradePower() { SetThreadExecutionState(ES_CONTINUOUS); }
    } power;
    const auto module = std::filesystem::path(ModulePath());
    const auto folder = module.parent_path();
    const auto root = folder.parent_path().parent_path();
    const auto ticket = folder / L"ticket.ini";
    std::error_code error;
    if (module.filename() != L"WindowsStreamUpdater.exe" || folder.parent_path().filename() != L".windowsstream-updates" ||
        !std::filesystem::equivalent(ticket, std::filesystem::path(ticketText), error) || error) return 2;
    const auto id = WideToUtf8(folder.filename().wstring());
    if (id.size() != 32 || id.find_first_not_of("0123456789abcdef") != std::string::npos) return 2;
    wchar_t tokenText[1024]{}, manifestText[256]{}, createdText[64]{};
    GetPrivateProfileStringW(L"connection", L"token", L"", tokenText, 1024, (root / L"host.ini").c_str());
    GetPrivateProfileStringW(L"update", L"manifest", L"", manifestText, 256, ticket.c_str());
    GetPrivateProfileStringW(L"update", L"created", L"", createdText, 64, ticket.c_str());
    const DWORD parentId = GetPrivateProfileIntW(L"update", L"parent", 0, ticket.c_str());
    const uint64_t expectedCreation = _wcstoui64(createdText, nullptr, 10);
    std::vector<uint8_t> manifest;
    uint64_t length = 0;
    uint32_t stamp = 0;
    std::array<uint8_t, 32> hash{};
    Package next, helper, old;
    const auto target = root / L"WindowsStreamHost.exe";
    const auto payload = folder / L"payload.exe";
    const auto backup = folder / L"previous.exe";
    if (!Unhex(WideToUtf8(manifestText), manifest) || !VerifyManifest(manifest, WideToUtf8(tokenText), length, stamp, hash) ||
        !LoadPackage(payload, next) || !LoadPackage(module, helper) || !LoadPackage(target, old) || next.bytes.size() != length ||
        next.stamp != stamp || stamp <= old.stamp || !SecureEqual(next.hash.data(), hash.data(), 32) ||
        !SecureEqual(helper.hash.data(), hash.data(), 32) || !parentId || !expectedCreation) return 3;
    const auto digest = Hex(hash.data(), hash.size());
    HANDLE parent = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, parentId);
    if (parent) {
        FILETIME created{}, exited{}, kernel{}, user{};
        std::wstring parentPath(32768, L'\0');
        DWORD characters = static_cast<DWORD>(parentPath.size());
        const bool valid = GetProcessTimes(parent, &created, &exited, &kernel, &user) &&
                           QueryFullProcessImageNameW(parent, 0, parentPath.data(), &characters);
        parentPath.resize(characters);
        const uint64_t creation = static_cast<uint64_t>(created.dwHighDateTime) << 32 | created.dwLowDateTime;
        if (!valid || creation != expectedCreation || !std::filesystem::equivalent(target, parentPath, error) || error) {
            CloseHandle(parent); return 4;
        }
        EnumWindows(CloseWindow, parentId);
        if (WaitForSingleObject(parent, 15000) == WAIT_TIMEOUT) {
            // 只结束已验证路径和创建时间的本次被控端，视频驱动卡住时也能恢复升级。
            if (!TerminateProcess(parent, 0) || WaitForSingleObject(parent, 5000) != WAIT_OBJECT_0) { CloseHandle(parent); return 5; }
        }
        CloseHandle(parent);
    } else if (GetLastError() != ERROR_INVALID_PARAMETER) return 4;
    if (!ReplaceFileW(target.c_str(), payload.c_str(), backup.c_str(), 0, nullptr, nullptr)) {
        State(root, L"替换失败，保留旧版", digest, true);
        RestoreDormantScreenSaver(root);
        PROCESS_INFORMATION restored{};
        if (Launch(target, L"", restored)) { CloseHandle(restored.hProcess); CloseHandle(restored.hThread); }
        return 6;
    }
    PROCESS_INFORMATION child{};
    bool healthy = false;
    if (Launch(target, L" --updated " + Utf8ToWide(id), child)) {
        CloseHandle(child.hThread);
        for (int i = 0; i < 150 && WaitForSingleObject(child.hProcess, 200) == WAIT_TIMEOUT; ++i) {
            wchar_t value[80]{};
            const auto health = folder / L"healthy.ini";
            GetPrivateProfileStringW(L"health", L"sha256", L"", value, 80, health.c_str());
            if (WideToUtf8(value) == digest && GetPrivateProfileIntW(L"health", L"pid", 0, health.c_str()) == child.dwProcessId) {
                healthy = true; break;
            }
        }
        if (!healthy && WaitForSingleObject(child.hProcess, 0) == WAIT_TIMEOUT) {
            TerminateProcess(child.hProcess, 0);
            WaitForSingleObject(child.hProcess, 5000);
        }
        CloseHandle(child.hProcess);
    }
    if (healthy) { State(root, L"升级成功", digest); return 0; }
    State(root, L"新版未通过升级通道健康检查，回退旧版", digest, true);
    if (!ReplaceFileW(target.c_str(), backup.c_str(), (folder / L"failed.exe").c_str(), 0, nullptr, nullptr)) {
        State(root, L"回退失败，需要检查更新目录", digest, true); RestoreDormantScreenSaver(root); return 7;
    }
    RestoreDormantScreenSaver(root);
    PROCESS_INFORMATION restored{};
    if (Launch(target, L"", restored)) { CloseHandle(restored.hProcess); CloseHandle(restored.hThread); }
    return 8;
}
}
