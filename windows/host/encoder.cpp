#include "encoder.h"
#include <codecapi.h>
#include <mferror.h>
#include <wmcodecdsp.h>

namespace {
bool Check(HRESULT result, const char* operation) {
    if (SUCCEEDED(result)) return true;
    LogLine("%s失败：0x%08X", operation, static_cast<unsigned>(result));
    return false;
}

void SetCodec(IMFTransform* transform, const GUID& key, ULONG value, VARTYPE type = VT_UI4) {
    ComPtr<ICodecAPI> codec;
    if (SUCCEEDED(transform->QueryInterface(IID_PPV_ARGS(&codec)))) {
        VARIANT setting{};
        setting.vt = type;
        if (type == VT_BOOL) setting.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
        else setting.ulVal = value;
        const HRESULT result = codec->SetValue(&key, &setting);
        if (FAILED(result)) LogLine("编码器可选参数未生效：0x%08X", static_cast<unsigned>(result));
    }
}

// BT.709 有限范围；色彩范围同时写进编码媒体类型，避免解码端按全范围解释。
void ConvertNv12(const uint8_t* bgra, int width, int height, std::vector<uint8_t>& output) {
    auto clamp = [](int value) { return static_cast<uint8_t>(std::clamp(value, 0, 255)); };
    for (int y = 0; y < height; y += 2) {
        for (int x = 0; x < width; x += 2) {
            int red = 0, green = 0, blue = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const auto offset = static_cast<size_t>(y + dy) * width + x + dx;
                    const auto* pixel = bgra + offset * 4;
                    output[offset] = clamp(((47 * pixel[2] + 157 * pixel[1] + 16 * pixel[0] + 128) >> 8) + 16);
                    red += pixel[2]; green += pixel[1]; blue += pixel[0];
                }
            }
            red = (red + 2) / 4; green = (green + 2) / 4; blue = (blue + 2) / 4;
            const size_t uv = static_cast<size_t>(width) * height + static_cast<size_t>(y / 2) * width + x;
            output[uv] = clamp(((-26 * red - 87 * green + 112 * blue + 128) >> 8) + 128);
            output[uv + 1] = clamp(((112 * red - 102 * green - 10 * blue + 128) >> 8) + 128);
        }
    }
}
}

VideoEncoder::~VideoEncoder() {
    if (encoder_) encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
}

bool VideoEncoder::Initialize(int width, int height, int fps, int bitrateMbps) {
    width_ = width; height_ = height; fps_ = fps;
    nv12_.resize(static_cast<size_t>(width) * height * 3 / 2);
    if (!Check(CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&encoder_)), "创建系统 H.264 编码器")) return false;
    SetCodec(encoder_.Get(), CODECAPI_AVLowLatencyMode, TRUE, VT_BOOL);
    SetCodec(encoder_.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
    SetCodec(encoder_.Get(), CODECAPI_AVEncMPVGOPSize, static_cast<ULONG>(fps * 2));
    ComPtr<IMFMediaType> output;
    if (!Check(MFCreateMediaType(&output), "创建编码输出类型")) return false;
    output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    output->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(bitrateMbps * 1000000));
    output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    output->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
    output->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    output->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(output.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(output.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (!Check(encoder_->SetOutputType(0, output.Get(), 0), "设置 H.264 输出格式")) return false;
    ComPtr<IMFMediaType> input;
    if (!Check(MFCreateMediaType(&input), "创建编码输入类型")) return false;
    output->CopyAllItems(input.Get());
    input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    input->DeleteItem(MF_MT_MPEG2_PROFILE);
    input->DeleteItem(MF_MT_AVG_BITRATE);
    input->SetUINT32(MF_MT_DEFAULT_STRIDE, width);
    if (!Check(encoder_->SetInputType(0, input.Get(), 0), "设置 NV12 输入格式")) return false;
    ComPtr<IMFMediaType> current;
    if (SUCCEEDED(encoder_->GetOutputCurrentType(0, &current))) {
        UINT32 bytes = 0;
        if (SUCCEEDED(current->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &bytes)) && bytes) {
            sequence_.resize(bytes);
            if (FAILED(current->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, sequence_.data(), bytes, nullptr))) sequence_.clear();
        }
    }
    if (!Check(encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "开始编码") ||
        !Check(encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "启动编码流")) return false;
    LogLine("系统软件编码器就绪：%dx%d，%d 帧，%d Mbps", width, height, fps, bitrateMbps);
    return true;
}

bool VideoEncoder::Encode(const uint8_t* bgra, uint64_t captureMicros, bool keyframe, const Sink& sink) {
    ConvertNv12(bgra, width_, height_, nv12_);
    return EncodeNv12(nv12_.data(), nv12_.size(), captureMicros, keyframe, sink);
}

bool VideoEncoder::EncodeNv12(const uint8_t* data, size_t size, uint64_t captureMicros, bool keyframe, const Sink& sink) {
    if (size != static_cast<size_t>(width_) * height_ * 3 / 2) return false;
    ComPtr<IMFMediaBuffer> buffer;
    if (!Check(MFCreateMemoryBuffer(static_cast<DWORD>(nv12_.size()), &buffer), "分配编码缓冲")) return false;
    BYTE* destination = nullptr;
    if (!Check(buffer->Lock(&destination, nullptr, nullptr), "锁定编码缓冲")) return false;
    memcpy(destination, data, size);
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(nv12_.size()));
    ComPtr<IMFSample> sample;
    if (!Check(MFCreateSample(&sample), "创建编码样本")) return false;
    sample->AddBuffer(buffer.Get());
    if (!firstMicros_) firstMicros_ = captureMicros;
    sample->SetSampleTime(static_cast<LONGLONG>(captureMicros - firstMicros_) * 10);
    sample->SetSampleDuration(10000000 / fps_);
    if (keyframe) SetCodec(encoder_.Get(), CODECAPI_AVEncVideoForceKeyFrame, TRUE);
    HRESULT result = encoder_->ProcessInput(0, sample.Get(), 0);
    if (result == MF_E_NOTACCEPTING) {
        if (!Drain(sink)) return false;
        result = encoder_->ProcessInput(0, sample.Get(), 0);
    }
    return Check(result, "提交编码画面") && Drain(sink);
}

bool VideoEncoder::Drain(const Sink& sink) {
    for (;;) {
        MFT_OUTPUT_STREAM_INFO info{};
        if (!Check(encoder_->GetOutputStreamInfo(0, &info), "读取编码输出信息")) return false;
        ComPtr<IMFSample> supplied;
        if (!(info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
            ComPtr<IMFMediaBuffer> buffer;
            const DWORD bytes = std::max<DWORD>(info.cbSize, static_cast<DWORD>(nv12_.size()));
            if (!Check(MFCreateSample(&supplied), "创建编码输出样本") ||
                !Check(MFCreateAlignedMemoryBuffer(bytes, info.cbAlignment ? info.cbAlignment - 1 : 0, &buffer), "分配编码输出缓冲")) return false;
            supplied->AddBuffer(buffer.Get());
        }
        MFT_OUTPUT_DATA_BUFFER output{};
        output.pSample = supplied.Get();
        DWORD status = 0;
        HRESULT result = encoder_->ProcessOutput(0, 1, &output, &status);
        if (output.pEvents) output.pEvents->Release();
        ComPtr<IMFSample> sample;
        if (output.pSample == supplied.Get()) sample = supplied;
        else sample.Attach(output.pSample);
        if (result == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
        if (!Check(result, "取得编码画面") || !sample) return false;
        ComPtr<IMFMediaBuffer> buffer;
        if (!Check(sample->ConvertToContiguousBuffer(&buffer), "合并编码缓冲")) return false;
        BYTE* data = nullptr;
        DWORD size = 0;
        if (!Check(buffer->Lock(&data, nullptr, &size), "读取编码数据")) return false;
        UINT32 keyframe = FALSE;
        LONGLONG timestamp = 0;
        sample->GetUINT32(MFSampleExtension_CleanPoint, &keyframe);
        if (!Check(sample->GetSampleTime(&timestamp), "读取画面时间戳")) {
            buffer->Unlock();
            return false;
        }
        std::vector<uint8_t> bytes;
        if (keyframe && !sequence_.empty()) bytes.insert(bytes.end(), sequence_.begin(), sequence_.end());
        bytes.insert(bytes.end(), data, data + size);
        buffer->Unlock();
        if (!sink(bytes.data(), bytes.size(), firstMicros_ + static_cast<uint64_t>(std::max<LONGLONG>(0, timestamp)) / 10,
                  keyframe != FALSE)) return false;
    }
}
