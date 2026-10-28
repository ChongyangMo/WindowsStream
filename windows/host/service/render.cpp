#include "service.h"

namespace servermode {
bool RenderLogon(HDC destination, int width, int height) {
    if (!SecureAgent()) return false;
    const auto window = FindWindowW(L"LogonUI Logon Window", nullptr);
    RECT area{};
    if (!window || !IsWindowVisible(window) || !GetWindowRect(window, &area)) return false;
    const int sourceWidth = area.right - area.left, sourceHeight = area.bottom - area.top;
    if (sourceWidth < 2 || sourceHeight < 2 || sourceWidth > 8192 || sourceHeight > 8192) return false;
    const auto memory = CreateCompatibleDC(destination);
    const auto bitmap = memory ? CreateCompatibleBitmap(destination, sourceWidth, sourceHeight) : nullptr;
    if (!bitmap) { if (memory) DeleteDC(memory); return false; }
    const auto previous = SelectObject(memory, bitmap);
    // 登录界面的合成内容在部分服务器显卡上不进入屏幕 DC，请窗口绘制真实内容。
    const bool rendered = PrintWindow(window, memory, 2) &&
        StretchBlt(destination, 0, 0, width, height, memory, 0, 0, sourceWidth, sourceHeight, SRCCOPY);
    SelectObject(memory, previous); DeleteObject(bitmap); DeleteDC(memory);
    static bool reported = false;
    if (rendered && !reported) { LogLine("服务器登录窗口已采用完整内容采集"); reported = true; }
    return rendered;
}
}
