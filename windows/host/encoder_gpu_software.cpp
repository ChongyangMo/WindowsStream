#include "encoder_gpu_software.h"

bool GpuSoftwareEncoder::Initialize(ID3D11Device* device, int sourceW, int sourceH,
                                    int width, int height, int fps, int bitrate) {
    device_ = device;
    device->GetImmediateContext(&context_);
    width_ = width; height_ = height;
    UINT token = 0;
    ComPtr<IMFMediaType> type;
    if (FAILED(MFCreateDXGIDeviceManager(&token, &manager_)) || FAILED(manager_->ResetDevice(device, token)) ||
        FAILED(MFCreateMediaType(&type))) return false;
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, fps, 1);
    if (!converter_.Initialize(device, manager_.Get(), type.Get(), sourceW, sourceH, width, height, fps)) return false;
    nv12_.resize(static_cast<size_t>(width) * height * 3 / 2);
    return encoder_.Initialize(width, height, fps, bitrate);
}

bool GpuSoftwareEncoder::Encode(ID3D11Texture2D* source, uint64_t timestamp, bool keyframe, const VideoEncoder::Sink& sink) {
    ComPtr<IMFSample> sample;
    if (FAILED(converter_.Convert(source, &sample))) return false;
    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMFDXGIBuffer> dxgi;
    ComPtr<ID3D11Texture2D> texture;
    UINT index = 0;
    if (FAILED(sample->GetBufferByIndex(0, &buffer)) || FAILED(buffer.As(&dxgi)) ||
        FAILED(dxgi->GetResource(IID_PPV_ARGS(&texture))) || FAILED(dxgi->GetSubresourceIndex(&index))) return false;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_NV12 || desc.Width < static_cast<UINT>(width_) || desc.Height < static_cast<UINT>(height_)) return false;
    if (!staging_) {
        auto copy = desc;
        copy.ArraySize = copy.MipLevels = 1;
        copy.Usage = D3D11_USAGE_STAGING;
        copy.BindFlags = copy.MiscFlags = 0;
        copy.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device_->CreateTexture2D(&copy, nullptr, &staging_))) return false;
    }
    context_->CopySubresourceRegion(staging_.Get(), 0, 0, 0, 0, texture.Get(), index, nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const auto result = context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result)) { LogLine("读取 GPU 转换帧失败：0x%08X", static_cast<unsigned>(result)); return false; }
    auto* data = static_cast<const uint8_t*>(mapped.pData);
    for (int row = 0; row < height_; ++row)
        memcpy(nv12_.data() + static_cast<size_t>(row) * width_, data + static_cast<size_t>(row) * mapped.RowPitch, width_);
    for (int row = 0; row < height_ / 2; ++row)
        memcpy(nv12_.data() + static_cast<size_t>(height_ + row) * width_,
               data + static_cast<size_t>(desc.Height + row) * mapped.RowPitch, width_);
    context_->Unmap(staging_.Get(), 0);
    return encoder_.EncodeNv12(nv12_.data(), nv12_.size(), timestamp, keyframe, sink);
}
