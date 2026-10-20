#include "cursor.h"

namespace {
bool RenderCursor(HCURSOR cursor, int width, int height, bool white, std::vector<uint8_t>& image) {
    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* pixels = nullptr;
    HBITMAP bitmap = memory ? CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0) : nullptr;
    HGDIOBJ previous = bitmap ? SelectObject(memory, bitmap) : nullptr;
    bool ok = false;
    if (pixels) {
        const auto bytes = static_cast<size_t>(width) * height * 4;
        memset(pixels, white ? 255 : 0, bytes);
        ok = DrawIconEx(memory, 0, 0, cursor, width, height, 0, nullptr, DI_NORMAL) != FALSE;
        GdiFlush();
        if (ok) image.assign(static_cast<uint8_t*>(pixels), static_cast<uint8_t*>(pixels) + bytes);
    }
    if (previous) SelectObject(memory, previous);
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return ok;
}
}

bool CursorSender::Poll(HostChannel& channel) {
    CURSORINFO cursor{sizeof cursor};
    if (!GetCursorInfo(&cursor)) return true;
    const bool visible = (cursor.flags & CURSOR_SHOWING) != 0;
    if (initialized_ && cursor.hCursor == previous_ && visible == visible_) return true;
    Writer payload;
    if (!visible) {
        payload.u16(1); payload.u16(1); payload.u16(0); payload.u16(0); payload.u32(0);
    } else {
        ICONINFO icon{};
        if (!GetIconInfo(cursor.hCursor, &icon)) return true;
        BITMAP bitmap{};
        const bool color = icon.hbmColor != nullptr;
        const bool valid = GetObjectW(color ? icon.hbmColor : icon.hbmMask, sizeof bitmap, &bitmap) != 0;
        const int width = bitmap.bmWidth;
        const int height = color ? bitmap.bmHeight : bitmap.bmHeight / 2;
        std::vector<uint8_t> black, white;
        bool ok = valid && width > 0 && width <= 256 && height > 0 && height <= 256 &&
                  RenderCursor(cursor.hCursor, width, height, false, black) && RenderCursor(cursor.hCursor, width, height, true, white);
        if (ok) {
            for (size_t i = 0; i < black.size(); i += 4) {
                const int transmission = std::max({0, white[i] - black[i], white[i + 1] - black[i + 1], white[i + 2] - black[i + 2]});
                black[i + 3] = static_cast<uint8_t>(255 - transmission);
            }
            payload.u16(static_cast<uint16_t>(width)); payload.u16(static_cast<uint16_t>(height));
            payload.u16(static_cast<uint16_t>(icon.xHotspot)); payload.u16(static_cast<uint16_t>(icon.yHotspot));
            payload.append(black.data(), black.size());
        }
        if (icon.hbmColor) DeleteObject(icon.hbmColor);
        if (icon.hbmMask) DeleteObject(icon.hbmMask);
        if (!ok) return true;
    }
    if (!channel.Send(MsgCursor, payload)) return false;
    previous_ = cursor.hCursor;
    visible_ = visible;
    initialized_ = true;
    return true;
}
