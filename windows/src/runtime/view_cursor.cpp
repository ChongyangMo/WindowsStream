#include "view_cursor.h"

HCURSOR DecodeRemoteCursor(const std::vector<uint8_t>& payload) {
    constexpr size_t headerSize = 8;
    if (payload.size() < headerSize) return nullptr;
    const int width = ReadU16(payload.data());
    const int height = ReadU16(payload.data() + 2);
    const DWORD hotX = ReadU16(payload.data() + 4);
    const DWORD hotY = ReadU16(payload.data() + 6);
    const size_t pixelBytes = static_cast<size_t>(width) * height * 4;
    if (width == 0 || height == 0 || payload.size() - headerSize < pixelBytes) return nullptr;

    const auto* pixels = payload.data() + headerSize;
    size_t alpha = 3;
    while (alpha < pixelBytes && pixels[alpha] == 0) alpha += 4;
    if (alpha >= pixelBytes) return nullptr;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = height;  // DIB 自下而上存放。
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color) return nullptr;

    // 远程像素使用预乘 alpha，创建光标前恢复颜色。
    const size_t rowBytes = static_cast<size_t>(width) * 4;
    for (int y = 0; y < height; ++y) {
        const auto* row = pixels + y * rowBytes;
        auto* dest = static_cast<uint8_t*>(bits) + (height - 1 - y) * rowBytes;
        for (size_t i = 0; i < rowBytes; i += 4) {
            const uint8_t opacity = row[i + 3];
            for (size_t channel = 0; channel < 3; ++channel) {
                dest[i + channel] = opacity ?
                    static_cast<uint8_t>(std::min(255, row[i + channel] * 255 / opacity)) : 0;
            }
            dest[i + 3] = opacity;
        }
    }

    std::vector<uint8_t> maskBits(static_cast<size_t>((width + 15) / 16 * 2) * height, 0);
    HBITMAP mask = CreateBitmap(width, height, 1, 1, maskBits.data());
    HCURSOR cursor = nullptr;
    if (mask) {
        ICONINFO icon{};
        icon.xHotspot = hotX;
        icon.yHotspot = hotY;
        icon.hbmMask = mask;
        icon.hbmColor = color;
        cursor = CreateIconIndirect(&icon);
        DeleteObject(mask);
    }
    DeleteObject(color);
    return cursor;
}
