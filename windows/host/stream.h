#pragma once
#include "capture.h"
#include "config.h"
#include "encoder_hw.h"
#include "encoder_gpu_software.h"
#include "../shared/telemetry.h"

struct StreamMetrics {
    uint64_t captured = 0, submitted = 0, skipped = 0, outputs = 0;
    uint64_t captureMicros = 0, submitMicros = 0, outputAgeMicros = 0;
    uint64_t submitAttempts = 0, outputAgeSamples = 0;
};

class FrameStream {
public:
    bool Initialize(const HostConfig& config);
    void Start() { if (hardware_) hardware_->Arm(); }
    bool Pump(const VideoEncoder::Sink& sink);
    bool Capture(bool keyframe, bool& submitted, const VideoEncoder::Sink& sink);
    int Width() const { return capture_.Width(); }
    int Height() const { return capture_.Height(); }
    int Fps() const { return fps_; }
    std::string Description() const;
    StreamMetrics TakeMetrics();
    unsigned Pending() const { return hardware_ ? hardware_->Pending() : 0; }
    HostTelemetry Telemetry() const;
    // 配置允许 GPU 采集却因安全桌面等临时原因退回 GDI；恢复后需重建视频链路。
    bool GpuFallback() const { return gpuWanted_ && !capture_.IsGpu(); }
    // 已正常出过画面后采集中断（合盖、屏幕关闭、桌面或分辨率切换），属于采集源变化而不是编码器故障。
    bool CaptureLost() const { return captureLost_; }
private:
    VideoEncoder::Sink Measure(const VideoEncoder::Sink& sink);
    DesktopCapture capture_;
    std::unique_ptr<HardwareEncoder> hardware_;
    std::unique_ptr<VideoEncoder> software_;
    std::unique_ptr<GpuSoftwareEncoder> gpuSoftware_;
    StreamMetrics metrics_;
    int fps_ = 0, requestedFps_ = 0;
    bool gpuWanted_ = false, captured_ = false, captureLost_ = false;
};

// 细粒度等待由系统定时器完成；不以忙等占用 CPU，也不依赖默认 15.6 ms 的线程睡眠粒度。
class StreamPacer {
public:
    StreamPacer();
    ~StreamPacer();
    void Wait(uint64_t microseconds);
private:
    HANDLE timer_ = nullptr;
    bool period_ = false;
};
