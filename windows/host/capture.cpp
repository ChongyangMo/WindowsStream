#include "capture.h"

DesktopCapture::~DesktopCapture() {
    Reset();
}

void DesktopCapture::Reset() {
    gpu_.reset();
    if (memory_ && previous_) SelectObject(memory_, previous_);
    if (bitmap_) DeleteObject(bitmap_);
    if (memory_) DeleteDC(memory_);
    if (screen_) ReleaseDC(nullptr, screen_);
    screen_ = memory_ = nullptr;
    bitmap_ = nullptr;
    previous_ = nullptr;
    pixels_ = nullptr;
    refreshHz_ = 0;
}

bool DesktopCapture::Initialize(int maxWidth, int maxHeight, bool gpu) {
    Reset();
    sourceWidth_ = GetSystemMetrics(SM_CXSCREEN);
    sourceHeight_ = GetSystemMetrics(SM_CYSCREEN);
    if (gpu) {
        auto candidate = std::make_unique<GpuCapture>();
        if (candidate->Initialize()) {
            sourceWidth_ = candidate->Width();
            sourceHeight_ = candidate->Height();
            refreshHz_ = candidate->RefreshHz();
            gpu_ = std::move(candidate);
        }
    }
    if (sourceWidth_ < 2 || sourceHeight_ < 2) return false;
    double scale = std::min({1.0, static_cast<double>(maxWidth) / sourceWidth_,
                            static_cast<double>(maxHeight) / sourceHeight_});
    width_ = std::max(2, static_cast<int>(sourceWidth_ * scale) & ~1);
    height_ = std::max(2, static_cast<int>(sourceHeight_ * scale) & ~1);
    if (gpu_) return true;
    DEVMODEW mode{};
    mode.dmSize = sizeof mode;
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1)
        refreshHz_ = static_cast<int>(mode.dmDisplayFrequency);
    screen_ = GetDC(nullptr);
    if (!screen_) return false;
    memory_ = CreateCompatibleDC(screen_);
    if (!memory_) return false;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width_;
    info.bmiHeader.biHeight = -height_;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    bitmap_ = CreateDIBSection(screen_, &info, DIB_RGB_COLORS, &pixels_, nullptr, 0);
    if (!bitmap_ || !pixels_) return false;
    previous_ = SelectObject(memory_, bitmap_);
    SetStretchBltMode(memory_, HALFTONE);
    SetBrushOrgEx(memory_, 0, 0, nullptr);
    LogLine("主屏采集：%dx%d，输出 %dx%d", sourceWidth_, sourceHeight_, width_, height_);
    return true;
}

bool DesktopCapture::Read() {
    if (GetSystemMetrics(SM_CXSCREEN) != sourceWidth_ || GetSystemMetrics(SM_CYSCREEN) != sourceHeight_) {
        LogLine("主屏分辨率发生变化，需要重新建立画面连接");
        return false;
    }
    // 安全桌面和锁屏不继续提供旧画面。
    HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    wchar_t name[128]{};
    DWORD needed = 0;
    bool accessible = desktop && GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof name, &needed) &&
                      _wcsicmp(name, L"Default") == 0;
    if (desktop) CloseDesktop(desktop);
    if (!accessible) {
        LogLine("当前桌面不可访问，可能处于锁屏或管理员确认界面");
        return false;
    }
    if (!StretchBlt(memory_, 0, 0, width_, height_, screen_, 0, 0, sourceWidth_, sourceHeight_, SRCCOPY | CAPTUREBLT)) {
        LogLine("采集桌面失败：%lu", GetLastError());
        return false;
    }
    GdiFlush();
    return true;
}

std::string DesktopCapture::Description() const {
    const auto name = gpu_ ? "GPU 桌面复制，显卡 " + WideToUtf8(gpu_->AdapterName()) : std::string("GDI 兼容采集");
    return name + "，主屏 " + std::to_string(sourceWidth_) + "×" + std::to_string(sourceHeight_) +
           "，刷新率 " + (refreshHz_ > 0 ? std::to_string(refreshHz_) + " Hz" : "未能读取");
}
