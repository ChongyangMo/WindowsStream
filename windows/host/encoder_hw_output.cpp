#include "encoder_hw.h"
#include <mferror.h>

namespace {
bool AnnexB(const uint8_t* data, size_t size) {
    return size >= 4 && data[0] == 0 && data[1] == 0 && (data[2] == 1 || (data[2] == 0 && data[3] == 1));
}
bool AppendLengthPrefixed(const uint8_t* data, size_t size, unsigned prefix, std::vector<uint8_t>& output) {
    if (!prefix || prefix > 4) return false;
    const auto original = output.size();
    while (size) {
        if (size <= prefix) { output.resize(original); return false; }
        uint32_t length = 0;
        for (unsigned i = 0; i < prefix; ++i) length = (length << 8) | data[i];
        data += prefix; size -= prefix;
        if (!length || length > size || (data[0] & 0x80) || !(data[0] & 31) || (data[0] & 31) > 23) {
            output.resize(original); return false;
        }
        output.insert(output.end(), {0, 0, 0, 1});
        output.insert(output.end(), data, data + length);
        data += length; size -= length;
    }
    return true;
}
}

void HardwareEncoder::LoadSequence() {
    ComPtr<IMFMediaType> type;
    if (FAILED(encoder_->GetOutputCurrentType(0, &type))) return;
    UINT32 length = 0;
    if (FAILED(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &length)) || !length || length > 65536) return;
    std::vector<uint8_t> data(length);
    if (FAILED(type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, data.data(), length, nullptr))) return;
    sequence_.clear();
    if (AnnexB(data.data(), data.size())) { sequence_ = std::move(data); return; }
    // 部分驱动提供 AVCDecoderConfigurationRecord，按记录中的真实长度字段转换。
    if (data.size() < 7 || data[0] != 1) return;
    nalLength_ = (data[4] & 3) + 1;
    size_t offset = 6;
    unsigned count = data[5] & 31;
    for (int group = 0; group < 2; ++group) {
        if (group) { if (offset >= data.size()) { sequence_.clear(); return; } count = data[offset++]; }
        for (unsigned i = 0; i < count; ++i) {
            if (offset + 2 > data.size()) { sequence_.clear(); return; }
            const size_t bytes = static_cast<size_t>(data[offset]) * 256 + data[offset + 1];
            offset += 2;
            if (!bytes || bytes > data.size() - offset) { sequence_.clear(); return; }
            sequence_.insert(sequence_.end(), {0, 0, 0, 1});
            sequence_.insert(sequence_.end(), data.begin() + offset, data.begin() + offset + bytes);
            offset += bytes;
        }
    }
}

bool HardwareEncoder::ReadOutput(const VideoEncoder::Sink& sink) {
    MFT_OUTPUT_STREAM_INFO info{};
    if (FAILED(encoder_->GetOutputStreamInfo(0, &info))) return false;
    ComPtr<IMFSample> supplied;
    if (!(info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(MFCreateSample(&supplied)) || FAILED(MFCreateAlignedMemoryBuffer(
            std::max<DWORD>(info.cbSize, 4 * 1024 * 1024), info.cbAlignment ? info.cbAlignment - 1 : 0, &buffer))) return false;
        supplied->AddBuffer(buffer.Get());
    }
    MFT_OUTPUT_DATA_BUFFER output{};
    output.pSample = supplied.Get();
    DWORD status = 0;
    const auto result = encoder_->ProcessOutput(0, 1, &output, &status);
    if (output.pEvents) output.pEvents->Release();
    ComPtr<IMFSample> sample;
    if (output.pSample == supplied.Get()) sample = supplied;
    else sample.Attach(output.pSample);
    if (result == MF_E_TRANSFORM_STREAM_CHANGE) {
        for (DWORD index = 0; index < 64; ++index) {
            ComPtr<IMFMediaType> type;
            if (FAILED(encoder_->GetOutputAvailableType(0, index, &type))) break;
            GUID subtype{};
            type->GetGUID(MF_MT_SUBTYPE, &subtype);
            if (subtype == MFVideoFormat_H264 && SUCCEEDED(encoder_->SetOutputType(0, type.Get(), 0))) {
                sequence_.clear(); LoadSequence();
                LogLine("硬件编码器输出格式已重新协商");
                // 异步 MFT 一次 HaveOutput 事件只允许一次 ProcessOutput。
                // 格式变化消费了当前事件，继续等待下一次事件，避免 Intel 返回 E_UNEXPECTED。
                lastActivity_ = NowMicros();
                return true;
            }
        }
    }
    if (result == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
    if (FAILED(result) || !sample) {
        LogLine("获取硬件编码输出失败：0x%08X", static_cast<unsigned>(result));
        return false;
    }
    if (pending_) --pending_;
    lastActivity_ = NowMicros();
    LONGLONG timestamp = 0;
    UINT32 keyframe = FALSE;
    if (FAILED(sample->GetSampleTime(&timestamp)) || timestamp < 0) return false;
    sample->GetUINT32(MFSampleExtension_CleanPoint, &keyframe);
    if (sequence_.empty()) LoadSequence();
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return false;
    BYTE* data = nullptr;
    DWORD size = 0;
    if (FAILED(buffer->Lock(&data, nullptr, &size))) return false;
    std::vector<uint8_t> bytes;
    if (keyframe) bytes = sequence_;
    bool valid = size > 0;
    if (valid && AnnexB(data, size)) bytes.insert(bytes.end(), data, data + size);
    else if (valid) valid = AppendLengthPrefixed(data, size, nalLength_ ? nalLength_ : 4, bytes);
    buffer->Unlock();
    if (!valid) { LogLine("硬件编码数据格式无法转换为 H.264 Annex-B"); return false; }
    return sink(bytes.data(), bytes.size(), firstMicros_ + static_cast<uint64_t>(timestamp) / 10, keyframe != FALSE);
}
