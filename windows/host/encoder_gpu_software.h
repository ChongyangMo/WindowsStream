#pragma once
#include "encoder.h"
#include "gpu_convert.h"

class GpuSoftwareEncoder {
public:
    bool Initialize(ID3D11Device* device, int sourceW, int sourceH, int width, int height, int fps, int bitrate);
    bool Encode(ID3D11Texture2D* source, uint64_t timestamp, bool keyframe, const VideoEncoder::Sink& sink);
private:
    VideoEncoder encoder_;
    GpuConverter converter_;
    ComPtr<IMFDXGIDeviceManager> manager_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11Texture2D> staging_;
    std::vector<uint8_t> nv12_;
    int width_ = 0, height_ = 0;
};
