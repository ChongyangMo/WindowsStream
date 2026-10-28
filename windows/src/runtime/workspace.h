#pragma once
#include "../common.h"

constexpr UINT kWorkspaceAction = WM_APP + 40;
constexpr UINT kViewerFocus = WM_APP + 41;
constexpr UINT kWorkspaceNew = 42101, kWorkspaceClose = 42102, kWorkspaceDetach = 42103, kWorkspaceNext = 42104;

struct ViewerStartup {
    std::wstring config;
    HWND workspace = nullptr;
    bool primary = false;
};

// 返回 -1 时继续运行单个观看进程，其余返回值由入口直接返回。
int RunControllerWorkspace(HINSTANCE instance, int show, ViewerStartup& viewer);
bool HandleWorkspaceShortcut(HWND viewer, UINT message, WPARAM key);
bool ViewerHasFocus(HWND viewer);
