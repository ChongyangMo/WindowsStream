#pragma once
#include "../src/common.h"
#include "capture_gpu.h"
#include <memory>

// 首版采集当前用户的主屏，不创建虚拟显示器，也不修改对方显示设置。
class DesktopCapture {
public:
    ~DesktopCapture();
    bool Initialize(int maxWidth, int maxHeight, bool gpu = true);
    bool Read();
    CaptureResult ReadGpu(uint64_t& timestamp) { return gpu_->Read(timestamp); }
    bool IsGpu() const { return gpu_ != nullptr; }
    GpuCapture* Gpu() const { return gpu_.get(); }
    int RefreshHz() const { return refreshHz_; }
    int SourceWidth() const { return sourceWidth_; }
    int SourceHeight() const { return sourceHeight_; }
    std::string Description() const;
    int Width() const { return width_; }
    int Height() const { return height_; }
    const uint8_t* Pixels() const { return static_cast<const uint8_t*>(pixels_); }
private:
    void Reset();
    std::unique_ptr<GpuCapture> gpu_;
    HDC screen_ = nullptr;
    HDC memory_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_ = nullptr;
    void* pixels_ = nullptr;
    int sourceWidth_ = 0, sourceHeight_ = 0;
    int width_ = 0, height_ = 0;
    int refreshHz_ = 0;
};
