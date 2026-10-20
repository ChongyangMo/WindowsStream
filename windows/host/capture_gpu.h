#pragma once
#include "../src/common.h"
#include <d3d11.h>
#include <dxgi1_2.h>

enum class CaptureResult { Frame, Idle, Failed };

class GpuCapture {
public:
    // log=false 用于后台探测 GPU 桌面复制是否恢复，不重复刷日志。
    bool Initialize(bool log = true);
    CaptureResult Read(uint64_t& timestamp);
    ID3D11Device* Device() const { return device_.Get(); }
    ID3D11Texture2D* Texture() const { return image_.Get(); }
    int Width() const { return width_; }
    int Height() const { return height_; }
    int RefreshHz() const { return refreshHz_; }
    bool HasFrame() const { return haveFrame_; }
    const std::wstring& AdapterName() const { return adapterName_; }
private:
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIOutputDuplication> duplication_;
    ComPtr<ID3D11Texture2D> image_;
    int width_ = 0, height_ = 0, refreshHz_ = 0;
    std::wstring adapterName_;
    bool haveFrame_ = false;
};
