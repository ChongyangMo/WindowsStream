#include "window.h"
#include "../profile.h"
#include <filesystem>

namespace workspace {
namespace {
HWND FindProcessWindow(DWORD process, HWND parent = nullptr) {
    struct Search { DWORD process; HWND result = nullptr; } search{process};
    const auto visit = [](HWND window, LPARAM data) -> BOOL {
        auto& search = *reinterpret_cast<Search*>(data);
        DWORD process = 0; GetWindowThreadProcessId(window, &process);
        wchar_t name[128]{}; GetClassNameW(window, name, 128);
        if (process == search.process && wcsncmp(name, L"WindowsStreamWindow", 19) == 0) { search.result = window; return FALSE; }
        return TRUE;
    };
    if (parent) EnumChildWindows(parent, visit, reinterpret_cast<LPARAM>(&search));
    else EnumWindows(visit, reinterpret_cast<LPARAM>(&search));
    return search.result;
}
}
void Window::Open(const Profile& profile) {
    for (auto workspace : windows) for (size_t i = 0; i < workspace->tabs_.size(); ++i) {
        if (CanonicalPath(workspace->tabs_[i].profile.path) == CanonicalPath(profile.path)) {
            ShowWindow(workspace->window_, SW_RESTORE); SetForegroundWindow(workspace->window_); workspace->Select(static_cast<int>(i)); return;
        }
    }
    Tab tab; tab.profile = profile; tab.started = GetTickCount64();
    // 先接入已在运行的旧窗口，保留视频、升级和内网代理连接。
    const auto identity = LoadControllerProfile(profile.path, !profile.primary);
    tab.view = FindWindowW(identity.windowClass.c_str(), nullptr);
    if (tab.view) {
        tab.borrowed = true; GetWindowThreadProcessId(tab.view, &tab.processId);
        tab.style = GetWindowLongPtrW(tab.view, GWL_STYLE); tab.exStyle = GetWindowLongPtrW(tab.view, GWL_EXSTYLE);
        GetWindowPlacement(tab.view, &tab.placement);
        LogLine("已将现有连接放入标签页：%s", WideToUtf8(profile.name).c_str());
    } else {
        const auto executable = ExecutablePath();
        std::wstring command = L"\"" + executable + L"\" --viewer \"" + profile.path + L"\" --workspace " +
            std::to_wstring(reinterpret_cast<UINT_PTR>(window_)) + (profile.primary ? L" --primary" : L"");
        STARTUPINFOW startup{sizeof startup}; startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process{};
        const auto directory = std::filesystem::path(executable).parent_path().wstring();
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, directory.c_str(), &startup, &process)) {
            LogLine("创建远程标签页失败：%lu", GetLastError());
            MessageBoxW(window_, L"无法打开连接窗口", L"WindowsStream", MB_ICONERROR); return;
        }
        tab.process = process.hProcess; tab.processId = process.dwProcessId; CloseHandle(process.hThread);
    }
    tabs_.push_back(std::move(tab)); selected_ = static_cast<int>(tabs_.size()) - 1;
    if (tabs_.back().view) Attach(tabs_.back());
    Refresh(); Select(selected_);
}
void Window::Attach(Tab& tab) {
    if (!tab.view || !IsWindow(tab.view)) return;
    if (!tab.borrowed) { tab.style = WS_OVERLAPPEDWINDOW; tab.exStyle = WS_EX_APPWINDOW; }
    tab.menu = GetMenu(tab.view); if (tab.menu) SetMenu(tab.view, nullptr);
    SetWindowLongPtrW(tab.view, GWL_STYLE, (GetWindowLongPtrW(tab.view, GWL_STYLE) & ~WS_OVERLAPPEDWINDOW) | WS_CHILD | WS_CLIPSIBLINGS);
    SetWindowLongPtrW(tab.view, GWL_EXSTYLE, GetWindowLongPtrW(tab.view, GWL_EXSTYLE) & ~WS_EX_APPWINDOW);
    SetParent(tab.view, window_);
    SetWindowPos(tab.view, nullptr, 0, HeaderHeight(), 1, 1, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    LogLine("远程标签页已就绪：%s", WideToUtf8(tab.profile.name).c_str());
}
void Window::Release(Tab& tab) {
    if (tab.menu && GetMenu(window_) == tab.menu) SetMenu(window_, nullptr);
    if (tab.view && IsWindow(tab.view)) {
        if (tab.borrowed || tab.profile.primary) {
            ShowWindow(tab.view, SW_HIDE); SetParent(tab.view, nullptr);
            SetWindowLongPtrW(tab.view, GWL_STYLE, tab.style);
            SetWindowLongPtrW(tab.view, GWL_EXSTYLE, tab.exStyle);
            if (tab.menu) SetMenu(tab.view, tab.menu);
            if (tab.borrowed) SetWindowPlacement(tab.view, &tab.placement);
            else SetWindowPos(tab.view, nullptr, 80, 80, 1200, 760, SWP_NOZORDER | SWP_FRAMECHANGED);
            SetWindowPos(tab.view, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            ShowWindowAsync(tab.view, SW_SHOWNORMAL);
            LogLine("主连接已保留，桌面和内网代理继续运行");
        } else PostMessageW(tab.view, WM_CLOSE, 0, 0);
    }
    if (tab.process) CloseHandle(tab.process);
    tab.process = nullptr;
}
void Window::CloseTab(int index) {
    if (index < 0 || static_cast<size_t>(index) >= tabs_.size()) return;
    LogLine("关闭远程标签页：%s", WideToUtf8(tabs_[static_cast<size_t>(index)].profile.name).c_str());
    Release(tabs_[static_cast<size_t>(index)]); tabs_.erase(tabs_.begin() + index);
    if (selected_ > index) --selected_;
    else if (selected_ >= static_cast<int>(tabs_.size())) selected_ = static_cast<int>(tabs_.size()) - 1;
    Refresh(); Select(selected_);
}
void Window::Detach(int index) {
    if (index < 0 || static_cast<size_t>(index) >= tabs_.size()) return;
    auto other = new Window(base_);
    if (!other->Create(SW_SHOWNORMAL)) return;
    auto tab = std::move(tabs_[static_cast<size_t>(index)]);
    if (tab.menu && GetMenu(window_) == tab.menu) SetMenu(window_, nullptr);
    tabs_.erase(tabs_.begin() + index);
    if (tab.view) SetParent(tab.view, other->window_);
    other->tabs_.push_back(std::move(tab)); other->selected_ = 0; other->Refresh(); other->Select(0);
    if (selected_ > index) --selected_;
    else if (selected_ >= static_cast<int>(tabs_.size())) selected_ = static_cast<int>(tabs_.size()) - 1;
    Refresh(); Select(selected_);
    LogLine("远程标签页已移到新窗口，连接保持不变");
}
void Window::Poll() {
    if (GetMenu(window_)) DrawMenuBar(window_);
    bool changed = false;
    for (auto& tab : tabs_) {
        if (tab.view && !IsWindow(tab.view)) { tab.view = nullptr; }
        if (!tab.view && tab.process && WaitForSingleObject(tab.process, 0) == WAIT_TIMEOUT) {
            tab.view = FindProcessWindow(tab.processId, window_);
            if (!tab.view) tab.view = FindProcessWindow(tab.processId);
            if (tab.view) { Attach(tab); changed = true; }
        }
    }
    // 已退出的观看进程只移除自己的标签页，不影响其他连接。
    for (size_t i = tabs_.size(); i > 0; --i) {
        const auto& tab = tabs_[i - 1];
        if ((!tab.view && tab.borrowed) || (tab.process && WaitForSingleObject(tab.process, 0) == WAIT_OBJECT_0)) {
            LogLine("远程标签页进程已退出：%s", WideToUtf8(tab.profile.name).c_str()); CloseTab(static_cast<int>(i - 1)); changed = true;
        }
    }
    if (changed) { Refresh(); Select(selected_); }
}
}
