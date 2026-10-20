#include "window_fit.h"

void FitStreamWindow(HWND window, int width, int height) {
    static uint64_t fitted = 0;
    const uint64_t key = static_cast<uint64_t>(width) << 32 | static_cast<uint32_t>(height);
    const auto style = static_cast<DWORD>(GetWindowLongW(window, GWL_STYLE));
    if (width <= 0 || height <= 0 || fitted == key || IsIconic(window) || IsZoomed(window) || !(style & WS_CAPTION)) return;
    MONITORINFO monitor{sizeof monitor};
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) return;
    const auto exStyle = static_cast<DWORD>(GetWindowLongW(window, GWL_EXSTYLE));
    RECT frame{};
    AdjustWindowRectExForDpi(&frame, style, GetMenu(window) != nullptr, exStyle, GetDpiForWindow(window));
    const int borderW = frame.right - frame.left, borderH = frame.bottom - frame.top;
    const int availableW = monitor.rcWork.right - monitor.rcWork.left - borderW - 20;
    const int availableH = monitor.rcWork.bottom - monitor.rcWork.top - borderH - 20;
    const double scale = std::min({1.0, availableW / static_cast<double>(width), availableH / static_cast<double>(height)});
    if (scale <= 0) return;
    const int windowW = static_cast<int>(width * scale) + borderW;
    const int windowH = static_cast<int>(height * scale) + borderH;
    RECT current{}; GetWindowRect(window, &current);
    const int x = std::clamp<int>((current.left + current.right - windowW) / 2, static_cast<int>(monitor.rcWork.left),
                             static_cast<int>(monitor.rcWork.right) - windowW);
    const int y = std::clamp<int>((current.top + current.bottom - windowH) / 2, static_cast<int>(monitor.rcWork.top),
                             static_cast<int>(monitor.rcWork.bottom) - windowH);
    fitted = key;
    SetWindowPos(window, nullptr, x, y, windowW, windowH, SWP_NOACTIVATE | SWP_NOZORDER);
    LogLine("观看窗口适配视频：%dx%d，缩放 %.3f", width, height, scale);
}
