#pragma once
#include "encoder.h"
#include "gpu_convert.h"
#include <memory>

class HardwareEncoder {
public:
    ~HardwareEncoder();
    bool Initialize(ID3D11Device* device, int sourceWidth, int sourceHeight,
                    int width, int height, int targetFps, int bitrateMbps);
    bool Pump(const VideoEncoder::Sink& sink);
    bool CanAccept() const { return inputCredits_ > 0 && pending_ < 4; }
    // 链路初始化后可能等待控制端连入很久；开始推流时重新计时，等待时间不算编码无进展。
    void Arm() { lastActivity_ = NowMicros(); }
    bool Encode(ID3D11Texture2D* bgra, uint64_t timestamp, bool keyframe, bool& submitted);
    int Fps() const { return fps_; }
    unsigned Pending() const { return pending_; }
    const std::string& Name() const { return name_; }
private:
    bool Select(ID3D11Device* device, int width, int height, int targetFps, int bitrateMbps);
    bool ReadOutput(const VideoEncoder::Sink& sink);
    void LoadSequence();
    void ResetTransform();
    ComPtr<IMFTransform> encoder_;
    ComPtr<IMFMediaEventGenerator> events_;
    ComPtr<IMFDXGIDeviceManager> manager_;
    ComPtr<IMFMediaType> inputType_;
    std::unique_ptr<GpuConverter> converter_;
    std::vector<uint8_t> sequence_;
    unsigned nalLength_ = 0;
    unsigned inputCredits_ = 0, pending_ = 0;
    uint64_t firstMicros_ = 0;
    uint64_t lastActivity_ = 0;
    int fps_ = 0;
    std::string name_;
};
