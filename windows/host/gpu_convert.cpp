#include "gpu_convert.h"

GpuConverter::~GpuConverter() {
    if (allocator_) allocator_->UninitializeSampleAllocator();
}

bool GpuConverter::Initialize(ID3D11Device* device, IMFDXGIDeviceManager* manager, IMFMediaType* inputType,
                              int sourceWidth, int sourceHeight, int width, int height, int fps) {
    device->GetImmediateContext(&context_);
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&videoDevice_))) || FAILED(context_.As(&videoContext_))) return false;
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
    desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    desc.InputWidth = sourceWidth; desc.InputHeight = sourceHeight;
    desc.OutputWidth = width; desc.OutputHeight = height;
    desc.InputFrameRate = desc.OutputFrameRate = {static_cast<UINT>(fps), 1};
    desc.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    HRESULT result = videoDevice_->CreateVideoProcessorEnumerator(&desc, &enumerator_);
    if (FAILED(result)) return false;
    UINT inputFlags = 0, outputFlags = 0;
    enumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &inputFlags);
    enumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &outputFlags);
    if (!(inputFlags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) ||
        !(outputFlags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) return false;
    if (FAILED(videoDevice_->CreateVideoProcessor(enumerator_.Get(), 0, &processor_))) return false;
    RECT source{0, 0, sourceWidth, sourceHeight};
    RECT target{0, 0, width, height};
    videoContext_->VideoProcessorSetStreamFrameFormat(processor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    videoContext_->VideoProcessorSetStreamAutoProcessingMode(processor_.Get(), 0, FALSE);
    videoContext_->VideoProcessorSetStreamSourceRect(processor_.Get(), 0, TRUE, &source);
    videoContext_->VideoProcessorSetStreamDestRect(processor_.Get(), 0, TRUE, &target);
    videoContext_->VideoProcessorSetOutputTargetRect(processor_.Get(), TRUE, &target);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE rgb{};
    rgb.RGB_Range = 0;
    rgb.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE nv12{};
    nv12.YCbCr_Matrix = 1;
    nv12.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    videoContext_->VideoProcessorSetStreamColorSpace(processor_.Get(), 0, &rgb);
    videoContext_->VideoProcessorSetOutputColorSpace(processor_.Get(), &nv12);
    D3D11_VIDEO_COLOR black{};
    black.YCbCr = {0.0625f, 0.5f, 0.5f, 1.0f};
    videoContext_->VideoProcessorSetOutputBackgroundColor(processor_.Get(), TRUE, &black);
    if (FAILED(MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&allocator_))) ||
        FAILED(allocator_->SetDirectXManager(manager))) return false;
    ComPtr<IMFAttributes> attributes;
    if (FAILED(MFCreateAttributes(&attributes, 2))) return false;
    attributes->SetUINT32(MF_SA_D3D11_BINDFLAGS, D3D11_BIND_RENDER_TARGET);
    attributes->SetUINT32(MF_SA_D3D11_USAGE, D3D11_USAGE_DEFAULT);
    result = allocator_->InitializeSampleAllocatorEx(4, 6, attributes.Get(), inputType);
    if (FAILED(result)) LogLine("创建 GPU 帧缓冲池失败：0x%08X", static_cast<unsigned>(result));
    return SUCCEEDED(result);
}

HRESULT GpuConverter::Convert(ID3D11Texture2D* source, IMFSample** output) {
    *output = nullptr;
    ComPtr<IMFSample> sample;
    HRESULT result = allocator_->AllocateSample(&sample);
    if (FAILED(result)) return result;
    if (lastSource_.Get() != source) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC view{};
        view.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        inputView_.Reset();
        result = videoDevice_->CreateVideoProcessorInputView(source, enumerator_.Get(), &view, &inputView_);
        if (FAILED(result)) return result;
        lastSource_ = source;
    }
    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMFDXGIBuffer> dxgi;
    ComPtr<ID3D11Texture2D> texture;
    UINT subresource = 0;
    if (FAILED(result = sample->GetBufferByIndex(0, &buffer)) || FAILED(result = buffer.As(&dxgi)) ||
        FAILED(result = dxgi->GetResource(IID_PPV_ARGS(&texture))) ||
        FAILED(result = dxgi->GetSubresourceIndex(&subresource))) return result;
    D3D11_TEXTURE2D_DESC textureDesc{};
    texture->GetDesc(&textureDesc);
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc{};
    if (textureDesc.ArraySize > 1) {
        outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;
        outputDesc.Texture2DArray.FirstArraySlice = subresource / textureDesc.MipLevels;
        outputDesc.Texture2DArray.ArraySize = 1;
    } else {
        outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        outputDesc.Texture2D.MipSlice = subresource;
    }
    ComPtr<ID3D11VideoProcessorOutputView> outputView;
    result = videoDevice_->CreateVideoProcessorOutputView(texture.Get(), enumerator_.Get(), &outputDesc, &outputView);
    if (FAILED(result)) return result;
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = inputView_.Get();
    result = videoContext_->VideoProcessorBlt(processor_.Get(), outputView.Get(), 0, 1, &stream);
    if (FAILED(result)) return result;
    context_->Flush();
    *output = sample.Detach();
    return S_OK;
}
