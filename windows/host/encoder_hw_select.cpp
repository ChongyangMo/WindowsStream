#include "encoder_hw.h"
#include <strmif.h>
#include <codecapi.h>
#include <dxgi.h>
#include <mferror.h>

namespace {
void SetOptional(IMFTransform* encoder, const char* name, const GUID& id, ULONG value, bool boolean = false) {
    ComPtr<ICodecAPI> codec;
    if (FAILED(encoder->QueryInterface(IID_PPV_ARGS(&codec)))) return;
    VARIANT setting{};
    setting.vt = static_cast<VARTYPE>(boolean ? VT_BOOL : VT_UI4);
    if (boolean) setting.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    else setting.ulVal = value;
    const auto result = codec->SetValue(&id, &setting);
    if (FAILED(result)) LogLine("硬件编码器参数未生效：%s，0x%08X", name, static_cast<unsigned>(result));
}

bool Configure(IMFTransform* encoder, IMFDXGIDeviceManager* manager, int width, int height,
               int fps, int bitrateMbps, IMFMediaType** inputType) {
    ComPtr<IMFAttributes> attributes;
    if (FAILED(encoder->GetAttributes(&attributes))) return false;
    UINT32 asynchronous = FALSE, aware = FALSE;
    attributes->GetUINT32(MF_TRANSFORM_ASYNC, &asynchronous);
    if (!asynchronous || FAILED(attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE))) return false;
    attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
    attributes->GetUINT32(MF_SA_D3D11_AWARE, &aware);
    if (!aware || FAILED(encoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(manager)))) return false;
    ComPtr<IMFMediaType> output, input;
    if (FAILED(MFCreateMediaType(&output)) || FAILED(MFCreateMediaType(&input))) return false;
    for (auto* type : {output.Get(), input.Get()}) {
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        MFSetAttributeSize(type, MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(type, MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    }
    output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    output->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(bitrateMbps * 1000000));
    output->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
    input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    input->SetUINT32(MF_MT_DEFAULT_STRIDE, width);
    if (FAILED(encoder->SetOutputType(0, output.Get(), 0)) || FAILED(encoder->SetInputType(0, input.Get(), 0))) return false;
    // 驱动拿到实际视频格式后再设置参数，部分硬件 MFT 在设置格式前会拒绝这些选项。
    SetOptional(encoder, "低延迟", CODECAPI_AVLowLatencyMode, TRUE, true);
    SetOptional(encoder, "实时编码", CODECAPI_AVEncCommonRealTime, TRUE, true);
    SetOptional(encoder, "关闭 B 帧", CODECAPI_AVEncMPVDefaultBPictureCount, 0);
    SetOptional(encoder, "关键帧间隔", CODECAPI_AVEncMPVGOPSize, static_cast<ULONG>(fps * 2));
    SetOptional(encoder, "固定码率", CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
    SetOptional(encoder, "平均码率", CODECAPI_AVEncCommonMeanBitRate, static_cast<ULONG>(bitrateMbps * 1000000));
    *inputType = input.Detach();
    return true;
}
}

void HardwareEncoder::ResetTransform() {
    events_.Reset();
    if (encoder_) {
        encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        ComPtr<IMFShutdown> shutdown;
        if (SUCCEEDED(encoder_.As(&shutdown))) shutdown->Shutdown();
    }
    encoder_.Reset();
    converter_.reset();
    inputType_.Reset();
    sequence_.clear();
    nalLength_ = inputCredits_ = pending_ = 0;
    firstMicros_ = 0;
}

bool HardwareEncoder::Select(ID3D11Device* device, int width, int height, int targetFps, int bitrateMbps) {
    UINT resetToken = 0;
    if (FAILED(MFCreateDXGIDeviceManager(&resetToken, &manager_)) || FAILED(manager_->ResetDevice(device, resetToken))) return false;
    MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, MFVideoFormat_H264};
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&desc))) return false;
    ComPtr<IMFAttributes> attributes;
    if (FAILED(MFCreateAttributes(&attributes, 1))) return false;
    attributes->SetBlob(MFT_ENUM_ADAPTER_LUID, reinterpret_cast<const UINT8*>(&desc.AdapterLuid), sizeof desc.AdapterLuid);
    IMFActivate** candidates = nullptr;
    UINT32 count = 0;
    const auto enumerate = reinterpret_cast<decltype(&MFTEnum2)>(GetProcAddress(GetModuleHandleW(L"mfplat.dll"), "MFTEnum2"));
    HRESULT result = enumerate ? enumerate(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        &input, &output, attributes.Get(), &candidates, &count) : E_NOTIMPL;
    if (FAILED(result) || !count) {
        if (candidates) { for (UINT32 i = 0; i < count; ++i) candidates[i]->Release(); CoTaskMemFree(candidates); }
        candidates = nullptr; count = 0;
        result = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                           &input, &output, &candidates, &count);
    }
    if (FAILED(result)) return false;
    bool selected = false;
    std::vector<int> rates{targetFps};
    if (targetFps > 60) rates.push_back(60);
    if (targetFps > 30) rates.push_back(30);
    for (int fps : rates) {
        for (UINT32 index = 0; index < count; ++index) {
            ResetTransform();
            wchar_t* friendly = nullptr;
            UINT32 characters = 0;
            candidates[index]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &friendly, &characters);
            const std::string name = friendly ? WideToUtf8(friendly) : std::string("未提供名称的硬件编码器");
            CoTaskMemFree(friendly);
            result = candidates[index]->ActivateObject(IID_PPV_ARGS(&encoder_));
            selected = SUCCEEDED(result) && Configure(encoder_.Get(), manager_.Get(), width, height, fps,
                                                      bitrateMbps, &inputType_) && SUCCEEDED(encoder_.As(&events_));
            if (selected) {
                fps_ = fps;
                name_ = name;
                break;
            }
            ResetTransform();
            candidates[index]->ShutdownObject();
            LogLine("硬件编码器未接受当前格式：%s，%dx%d，%d 帧", name.c_str(), width, height, fps);
        }
        if (selected) break;
    }
    for (UINT32 index = 0; index < count; ++index) candidates[index]->Release();
    CoTaskMemFree(candidates);
    return selected;
}
