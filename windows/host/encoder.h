#pragma once
#include "../src/common.h"
#include <mfapi.h>
#include <mftransform.h>
#include <functional>

// 首版使用系统 H.264 软件编码器；控制端沿用 D3D11 硬件解码。
class VideoEncoder {
public:
    using Sink = std::function<bool(const uint8_t*, size_t, uint64_t, bool)>;
    ~VideoEncoder();
    bool Initialize(int width, int height, int fps, int bitrateMbps);
    bool Encode(const uint8_t* bgra, uint64_t captureMicros, bool keyframe, const Sink& sink);
    bool EncodeNv12(const uint8_t* data, size_t size, uint64_t captureMicros, bool keyframe, const Sink& sink);
private:
    bool Drain(const Sink& sink);
    ComPtr<IMFTransform> encoder_;
    std::vector<uint8_t> nv12_;
    std::vector<uint8_t> sequence_;
    int width_ = 0, height_ = 0, fps_ = 30;
    uint64_t firstMicros_ = 0;
};
