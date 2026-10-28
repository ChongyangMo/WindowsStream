#include "workspace.h"
#include "workspace/window.h"
#include <shellapi.h>
#include <filesystem>

int RunControllerWorkspace(HINSTANCE instance, int show, ViewerStartup& viewer) {
    int count = 0; auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    std::wstring initial;
    ULONG_PTR operation = 1;
    bool newConnection = false;
    bool replace = false;
    for (int i = 1; arguments && i < count; ++i) {
        const std::wstring arg = arguments[i];
        if (arg == L"--viewer" && i + 1 < count) viewer.config = arguments[++i];
        else if (arg == L"--workspace" && i + 1 < count) viewer.workspace = reinterpret_cast<HWND>(static_cast<UINT_PTR>(_wcstoui64(arguments[++i], nullptr, 10)));
        else if (arg == L"--primary") viewer.primary = true;
        else if (arg == L"--profile" && i + 1 < count) initial = arguments[++i];
        else if (arg == L"--detach-profile" && i + 1 < count) { initial = arguments[++i]; operation = 2; }
        else if (arg == L"--close-profile" && i + 1 < count) { initial = arguments[++i]; operation = 3; }
        else if (arg == L"--new-window") newConnection = true;
        else if (arg == L"--replace-workspace") replace = true;
        else if (arg == L"--classic") { viewer.config = workspace::DefaultConfig(); viewer.primary = true; }
        else if (arg.size() && arg[0] != L'-') initial = arg;
    }
    if (arguments) LocalFree(arguments);
    if (!viewer.config.empty()) {
        viewer.config = workspace::CanonicalPath(viewer.config);
        SetRuntimeDirectory(std::filesystem::path(viewer.config).parent_path().wstring());
        return -1;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto config = workspace::DefaultConfig();
    if (!initial.empty() && GetFileAttributesW(initial.c_str()) == INVALID_FILE_ATTRIBUTES) {
        workspace::Profile profile; std::wstring error;
        if (!workspace::SaveProfile(config, initial, 7400, workspace::ReadSetting(config, L"connection", L"token"), profile, error)) {
            MessageBoxW(nullptr, error.c_str(), L"WindowsStream", MB_ICONERROR); return 1;
        }
        initial = profile.path;
    }
    if (const auto existing = FindWindowW(workspace::kWindowClass, nullptr)) {
        if (replace) {
            // 只重开标签页外壳；已有主连接恢复为独立窗口后再接入新版。
            HWND current = existing;
            while (current) {
                PostMessageW(current, WM_CLOSE, 0, 0);
                const auto deadline = GetTickCount64() + 5000;
                while (IsWindow(current) && GetTickCount64() < deadline) Sleep(50);
                if (IsWindow(current)) { LogLine("标签页外壳仍在运行，未停止任何远程连接"); return 1; }
                current = FindWindowW(workspace::kWindowClass, nullptr);
            }
        } else {
            COPYDATASTRUCT data{operation, static_cast<DWORD>((initial.size() + 1) * sizeof(wchar_t)), initial.data()};
            DWORD_PTR result = 0;
            if (SendMessageTimeoutW(existing, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, 3000, &result) && result) return 0;
            LogLine("现有标签页窗口未响应，保留原连接"); return 1;
        }
    }
    if (operation != 1) return 1;
    INITCOMMONCONTROLSEX controls{sizeof controls, ICC_TAB_CLASSES}; InitCommonControlsEx(&controls);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const auto keyboard = SetWindowsHookExW(WH_KEYBOARD_LL, workspace::Window::Keyboard, GetModuleHandleW(nullptr), 0);
    auto window = new workspace::Window(config);
    if (!window->Create(show)) { if (keyboard) UnhookWindowsHookEx(keyboard); CoUninitialize(); return 1; }
    if (!initial.empty()) window->Open({workspace::ReadSetting(initial, L"connection", L"name", L"WindowsStream"), initial,
        workspace::CanonicalPath(initial) == workspace::CanonicalPath(config) && workspace::LoadProfiles(config).front().primary});
    else if (!newConnection) window->Open(workspace::LoadProfiles(config).front());
    else PostMessageW(FindWindowW(workspace::kWindowClass, nullptr), kWorkspaceAction, kWorkspaceNew, 0);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        bool handled = false;
        const auto active = workspace::windows;
        for (auto current : active) if (current->Translate(message)) { handled = true; break; }
        if (!handled) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    if (keyboard) UnhookWindowsHookEx(keyboard);
    CoUninitialize();
    (void)instance;
    return 0;
}
bool ViewerHasFocus(HWND viewer) {
    return IsWindowVisible(viewer) && GetForegroundWindow() == GetAncestor(viewer, GA_ROOT);
}
bool HandleWorkspaceShortcut(HWND viewer, UINT message, WPARAM key) {
    if (!(GetWindowLongPtrW(viewer, GWL_STYLE) & WS_CHILD)) return false;
    const auto parent = GetAncestor(viewer, GA_ROOT);
    wchar_t name[128]{}; GetClassNameW(parent, name, 128);
    if (wcscmp(name, workspace::kWindowClass) != 0 || !(GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000)) return false;
    const UINT command = key == 'T' || key == 'N' ? kWorkspaceNew : key == 'W' ? kWorkspaceClose : key == VK_TAB ? kWorkspaceNext : 0;
    if (!command) return false;
    if (message == WM_KEYDOWN && !(GetKeyState(VK_MENU) & 0x8000))
        PostMessageW(parent, kWorkspaceAction, command, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
    return true;
}
