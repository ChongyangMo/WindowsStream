#include "network_setup.h"
#include "workspace/catalog.h"
#include "../../resources/resource.h"
#include <shellapi.h>
#include <shlobj.h>
#include <wincrypt.h>
#include <string_view>

namespace network_setup {
namespace {
constexpr auto kCommand = L"--network-setup";
struct Action { UINT id; const wchar_t* name; const wchar_t* label; };
constexpr Action kActions[] = {
    {42003, L"wired", L"配置网线直连"},
    {42004, L"vm", L"放行虚拟机代理"},
    {42005, L"remove-vm", L"撤销虚拟机代理放行"},
};

std::wstring Encode(std::wstring_view value) {
    const auto bytes = reinterpret_cast<const BYTE*>(value.data());
    const auto size = static_cast<DWORD>(value.size() * sizeof(wchar_t));
    constexpr DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
    DWORD chars = 0;
    if (!CryptBinaryToStringW(bytes, size, flags, nullptr, &chars)) return {};
    std::wstring result(chars, L'\0');
    if (!CryptBinaryToStringW(bytes, size, flags, result.data(), &chars)) return {};
    result.resize(chars);
    return result;
}

int Fail(const wchar_t* message) {
    LogLine("%s", WideToUtf8(message).c_str());
    MessageBoxW(nullptr, message, L"WindowsStream", MB_ICONERROR);
    return 1;
}

int Configure(const Action& action) {
    if (!IsUserAnAdmin()) return Fail(L"请从托盘的「网络配置」打开此功能，并确认管理员权限。");
    const auto module = GetModuleHandleW(nullptr);
    const auto resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_NETWORK_SETUP), RT_RCDATA);
    const auto size = resource ? SizeofResource(module, resource) : 0;
    const auto data = resource ? static_cast<const char*>(LockResource(LoadResource(module, resource))) : nullptr;
    if (!data || !size) return Fail(L"无法读取网络配置组件。");

    const auto executable = Encode(workspace::ExecutablePath());
    if (executable.empty()) return Fail(L"无法读取控制端程序位置。");
    const auto script = L"$action = '" + std::wstring(action.name) +
        L"'\r\n$program = [Text.Encoding]::Unicode.GetString([Convert]::FromBase64String('" + executable +
        L"'))\r\n" + Utf8ToWide(std::string(data, size));
    const auto encoded = Encode(script);
    if (encoded.empty()) return Fail(L"无法准备网络配置组件。");

    wchar_t directory[MAX_PATH]{};
    const auto length = GetSystemDirectoryW(directory, MAX_PATH);
    if (!length || length >= MAX_PATH) return Fail(L"无法读取 Windows 系统目录。");
    const auto powershell = std::wstring(directory, length) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    auto command = L"\"" + powershell + L"\" -NoProfile -NonInteractive -EncodedCommand " + encoded;
    STARTUPINFOW startup{sizeof startup};
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(powershell.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, directory, &startup, &child)) return Fail(L"无法启动 Windows 网络配置组件。");
    CloseHandle(child.hThread);
    WaitForSingleObject(child.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(child.hProcess, &exitCode);
    CloseHandle(child.hProcess);
    LogLine("网络配置 %ls，结果码 %lu", action.label, exitCode);
    return exitCode == 0 ? 0 : 1;
}
}

void AddTrayMenu(HMENU tray) {
    const auto menu = CreatePopupMenu();
    if (!menu) return;
    for (const auto& action : kActions) {
        if (!AppendMenuW(menu, MF_STRING, action.id, action.label)) { DestroyMenu(menu); return; }
    }
    if (!AppendMenuW(tray, MF_POPUP, reinterpret_cast<UINT_PTR>(menu), L"网络配置")) DestroyMenu(menu);
}

bool HandleTrayCommand(HWND owner, UINT command) {
    for (const auto& action : kActions) {
        if (action.id != command) continue;
        const auto executable = workspace::ExecutablePath();
        const auto args = std::wstring(kCommand) + L" " + action.name;
        SHELLEXECUTEINFOW request{sizeof request};
        request.fMask = SEE_MASK_FLAG_NO_UI;
        request.hwnd = owner;
        request.lpVerb = L"runas";
        request.lpFile = executable.c_str();
        request.lpParameters = args.c_str();
        request.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&request) && GetLastError() != ERROR_CANCELLED) {
            LogLine("无法以管理员权限打开网络配置");
            MessageBoxW(owner, L"无法以管理员权限打开网络配置。", L"WindowsStream", MB_ICONERROR);
        }
        return true;
    }
    return false;
}

int Dispatch() {
    int count = 0;
    const auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    const Action* action = nullptr;
    const bool requested = arguments && count >= 2 && std::wstring_view(arguments[1]) == kCommand;
    if (requested && count == 3) {
        for (const auto& item : kActions) if (std::wstring_view(arguments[2]) == item.name) action = &item;
    }
    if (arguments) LocalFree(arguments);
    if (!requested) return -1;
    return action ? Configure(*action) : 1;
}
}
