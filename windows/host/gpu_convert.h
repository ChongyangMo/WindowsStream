#pragma once
#include "../src/common.h"
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>

class GpuConverter {
public:
    ~GpuConverter();
    bool Initialize(ID3D11Device* device, IMFDXGIDeviceManager* manager, IMFMediaType* inputType,
                    int sourceWidth, int sourceHeight, int width, int height, int fps);
    HRESULT Convert(ID3D11Texture2D* source, IMFSample** sample);
private:
    ComPtr<ID3D11VideoDevice> videoDevice_;
    ComPtr<ID3D11VideoContext> videoContext_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
    ComPtr<ID3D11VideoProcessor> processor_;
    ComPtr<IMFVideoSampleAllocatorEx> allocator_;
    ComPtr<ID3D11VideoProcessorInputView> inputView_;
    ComPtr<ID3D11Texture2D> lastSource_;
};
