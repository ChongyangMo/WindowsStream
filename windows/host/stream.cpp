#include "stream.h"
#include <mmsystem.h>
#include <utility>

bool FrameStream::Initialize(const HostConfig& config) {
    requestedFps_ = config.fps;
    const bool useGpu = config.capture != L"gdi";
    gpuWanted_ = useGpu;
    const int maxWidth = config.nativeResolution ? 3840 : config.maxWidth;
    const int maxHeight = config.nativeResolution ? 2160 : config.maxHeight;
    LogLine("输出分辨率策略：%s", config.nativeResolution ? "原生主屏，最高 3840×2160" : "按配置上限缩放");
    if (!capture_.Initialize(maxWidth, maxHeight, useGpu)) return false;
    if (config.capture == L"dxgi" && !capture_.IsGpu()) {
        LogLine("配置要求 GPU 桌面采集，但当前设备无法启用");
        return false;
    }
    fps_ = capture_.RefreshHz() > 1 ? std::min(config.fps, capture_.RefreshHz()) : config.fps;
    if (capture_.IsGpu() && config.encoder != L"software") {
        hardware_ = std::make_unique<HardwareEncoder>();
        auto* gpu = capture_.Gpu();
        if (hardware_->Initialize(gpu->Device(), gpu->Width(), gpu->Height(), Width(), Height(), fps_, config.bitrateMbps)) {
            fps_ = hardware_->Fps();
            return true;
        }
        hardware_.reset();
    }
    if (config.encoder == L"hardware") {
        LogLine("配置要求的硬件编码路径不可用，请检查显卡驱动和 host.ini");
        return false;
    }
    if (capture_.IsGpu()) {
        auto* gpu = capture_.Gpu();
        gpuSoftware_ = std::make_unique<GpuSoftwareEncoder>();
        if (gpuSoftware_->Initialize(gpu->Device(), gpu->Width(), gpu->Height(), Width(), Height(), fps_, config.bitrateMbps)) {
            LogLine("已启用 GPU 采集和色彩转换，使用系统软件编码，上限 %d 帧", fps_);
            return true;
        }
        gpuSoftware_.reset();
        LogLine("GPU 软件编码兼容路径不可用，将使用 GDI 兼容采集");
    }
    if (!capture_.Initialize(maxWidth, maxHeight, false)) return false;
    fps_ = std::min(config.fps, 30);
    if (capture_.RefreshHz() > 1) fps_ = std::min(fps_, capture_.RefreshHz());
    LogLine("使用软件兼容路径，上限 %d 帧；此路径不保证达到目标 %d 帧", fps_, config.fps);
    software_ = std::make_unique<VideoEncoder>();
    return software_->Initialize(Width(), Height(), fps_, config.bitrateMbps);
}

VideoEncoder::Sink FrameStream::Measure(const VideoEncoder::Sink& sink) {
    return [this, &sink](const uint8_t* data, size_t size, uint64_t timestamp, bool keyframe) {
        const auto now = NowMicros();
        ++metrics_.outputs;
        if (now >= timestamp) { metrics_.outputAgeMicros += now - timestamp; ++metrics_.outputAgeSamples; }
        return sink(data, size, timestamp, keyframe);
    };
}

bool FrameStream::Pump(const VideoEncoder::Sink& sink) {
    return !hardware_ || hardware_->Pump(Measure(sink));
}

bool FrameStream::Capture(bool keyframe, bool& submitted, const VideoEncoder::Sink& sink) {
    submitted = false;
    if (hardware_ && !hardware_->CanAccept()) { ++metrics_.skipped; return true; }
    uint64_t timestamp = 0;
    const auto before = NowMicros();
    if (capture_.IsGpu()) {
        const auto result = capture_.ReadGpu(timestamp);
        if (result == CaptureResult::Failed) { captureLost_ = captured_; return false; }
        if (result == CaptureResult::Idle) {
            if (!keyframe || !capture_.Gpu()->HasFrame()) return true;
            timestamp = NowMicros();  // 关键帧请求可以重新编码真实缓存桌面，不周期性制造重复帧。
        }
    } else {
        if (!capture_.Read()) { captureLost_ = captured_; return false; }
        timestamp = NowMicros();
    }
    captured_ = true;
    metrics_.captureMicros += NowMicros() - before;
    ++metrics_.captured;
    const auto start = NowMicros();
    ++metrics_.submitAttempts;
    if (hardware_) {
        if (!hardware_->Encode(capture_.Gpu()->Texture(), timestamp, keyframe, submitted)) return false;
        if (!submitted) ++metrics_.skipped;
    } else if (gpuSoftware_) {
        if (!gpuSoftware_->Encode(capture_.Gpu()->Texture(), timestamp, keyframe, Measure(sink))) return false;
        submitted = true;
    } else {
        if (!software_->Encode(capture_.Pixels(), timestamp, keyframe, Measure(sink))) return false;
        submitted = true;
    }
    metrics_.submitMicros += NowMicros() - start;
    if (submitted) ++metrics_.submitted;
    return true;
}

std::string FrameStream::Description() const {
    return capture_.Description() + "；编码 " + (hardware_ ? hardware_->Name() : gpuSoftware_ ? "GPU 转换和系统软件编码" : "系统软件编码") +
           "；输出 " + std::to_string(Width()) + "×" + std::to_string(Height()) +
           "；请求 " + std::to_string(requestedFps_) + " 帧，协商上限 " + std::to_string(fps_) + " 帧";
}

StreamMetrics FrameStream::TakeMetrics() { return std::exchange(metrics_, {}); }

HostTelemetry FrameStream::Telemetry() const {
    HostTelemetry result;
    result.sourceWidth = static_cast<uint16_t>(capture_.SourceWidth());
    result.sourceHeight = static_cast<uint16_t>(capture_.SourceHeight());
    result.refreshHz = static_cast<uint16_t>(capture_.RefreshHz());
    result.width = static_cast<uint16_t>(Width()); result.height = static_cast<uint16_t>(Height());
    result.fpsCap = static_cast<uint16_t>(Fps());
    result.pipeline = hardware_ ? 1 : gpuSoftware_ ? 2 : 3;
    result.pending = Pending();
    return result;
}

StreamPacer::StreamPacer() {
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer_) {
        timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        period_ = timeBeginPeriod(1) == TIMERR_NOERROR;
    }
}

StreamPacer::~StreamPacer() {
    if (timer_) CloseHandle(timer_);
    if (period_) timeEndPeriod(1);
}

void StreamPacer::Wait(uint64_t microseconds) {
    microseconds = std::clamp<uint64_t>(microseconds, 100, 1000);
    LARGE_INTEGER due{};
    due.QuadPart = -static_cast<LONGLONG>(microseconds * 10);
    if (timer_ && SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer_, 100);
    else Sleep(1);
}
