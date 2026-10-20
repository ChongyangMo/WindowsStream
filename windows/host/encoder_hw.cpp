#include "encoder_hw.h"
#include <strmif.h>
#include <codecapi.h>
#include <mferror.h>

HardwareEncoder::~HardwareEncoder() { ResetTransform(); }

bool HardwareEncoder::Initialize(ID3D11Device* device, int sourceWidth, int sourceHeight,
                                  int width, int height, int targetFps, int bitrateMbps) {
    if (!Select(device, width, height, targetFps, bitrateMbps)) return false;
    converter_ = std::make_unique<GpuConverter>();
    if (!converter_->Initialize(device, manager_.Get(), inputType_.Get(), sourceWidth, sourceHeight, width, height, fps_)) {
        LogLine("GPU 色彩转换初始化失败");
        ResetTransform();
        return false;
    }
    LoadSequence();
    if (FAILED(encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)) ||
        FAILED(encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0))) {
        ResetTransform();
        return false;
    }
    lastActivity_ = NowMicros();
    LogLine("硬件编码已启用：%s，%dx%d，协商 %d 帧，%d Mbps", name_.c_str(), width, height, fps_, bitrateMbps);
    return true;
}

bool HardwareEncoder::Pump(const VideoEncoder::Sink& sink) {
    if ((pending_ || !firstMicros_ || !inputCredits_) && NowMicros() - lastActivity_ > 3000000) {
        LogLine("硬件编码超过三秒没有进展，退出当前链路并触发兼容回退");
        return false;
    }
    // 所有 MFT 调用在采集线程串行执行，按异步事件额度提交，不盲目轮询 ProcessInput。
    for (int i = 0; i < 64; ++i) {
        ComPtr<IMFMediaEvent> event;
        const auto result = events_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
        if (result == MF_E_NO_EVENTS_AVAILABLE) return true;
        if (FAILED(result)) { LogLine("读取硬件编码事件失败：0x%08X", static_cast<unsigned>(result)); return false; }
        MediaEventType type = MEUnknown;
        HRESULT status = S_OK;
        event->GetType(&type);
        event->GetStatus(&status);
        if (FAILED(status)) { LogLine("硬件编码设备报告失败：0x%08X", static_cast<unsigned>(status)); return false; }
        if (type == METransformNeedInput) ++inputCredits_;
        else if (type == METransformHaveOutput && !ReadOutput(sink)) return false;
        else if (type == MEError) { LogLine("硬件编码器报告错误事件"); return false; }
    }
    return true;
}

bool HardwareEncoder::Encode(ID3D11Texture2D* bgra, uint64_t timestamp, bool keyframe, bool& submitted) {
    submitted = false;
    if (!CanAccept()) return true;
    ComPtr<IMFSample> sample;
    auto result = converter_->Convert(bgra, &sample);
    if (result == MF_E_SAMPLEALLOCATOR_EMPTY) return true;
    if (FAILED(result)) { LogLine("GPU 帧转换失败：0x%08X", static_cast<unsigned>(result)); return false; }
    if (!firstMicros_) firstMicros_ = timestamp;
    sample->SetSampleTime(static_cast<LONGLONG>(timestamp - firstMicros_) * 10);
    sample->SetSampleDuration(10000000 / fps_);
    if (keyframe) {
        ComPtr<ICodecAPI> codec;
        if (SUCCEEDED(encoder_.As(&codec))) {
            VARIANT value{};
            value.vt = VT_UI4; value.ulVal = TRUE;
            codec->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &value);
        }
    }
    result = encoder_->ProcessInput(0, sample.Get(), 0);
    if (result == MF_E_NOTACCEPTING) { inputCredits_ = 0; return true; }
    if (FAILED(result)) { LogLine("提交 GPU 编码帧失败：0x%08X", static_cast<unsigned>(result)); return false; }
    --inputCredits_;
    ++pending_;
    lastActivity_ = NowMicros();
    submitted = true;
    return true;
}
