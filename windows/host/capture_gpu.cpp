#include "capture_gpu.h"
#include "capture_diag.h"
#include <dxgi1_5.h>

bool GpuCapture::Initialize(bool log) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    POINT origin{};
    const auto primary = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    ComPtr<IDXGIAdapter1> selectedAdapter;
    ComPtr<IDXGIOutput1> selectedOutput;
    DXGI_OUTPUT_DESC outputDesc{};
    for (UINT adapterIndex = 0; !selectedOutput; ++adapterIndex) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(adapterIndex, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        if (!adapter) break;
        for (UINT outputIndex = 0;; ++outputIndex) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(outputIndex, &output) == DXGI_ERROR_NOT_FOUND) break;
            if (!output || FAILED(output->GetDesc(&outputDesc))) break;
            if (outputDesc.AttachedToDesktop && outputDesc.Monitor == primary) {
                selectedAdapter = adapter;
                output.As(&selectedOutput);
                break;
            }
        }
    }
    if (!selectedOutput || outputDesc.Rotation != DXGI_MODE_ROTATION_IDENTITY) {
        if (log) LogLine("主屏不支持当前 GPU 采集路径，使用兼容采集");
        return false;
    }
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT result = D3D11CreateDevice(selectedAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        levels, 2, D3D11_SDK_VERSION, &device_, nullptr, &context_);
    if (FAILED(result)) {
        if (log) LogLine("创建采集显卡设备失败：0x%08X", static_cast<unsigned>(result));
        return false;
    }
    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(device_.As(&multithread))) multithread->SetMultithreadProtected(TRUE);
    result = selectedOutput->DuplicateOutput(device_.Get(), &duplication_);
    if (FAILED(result)) {
        // 旧接口被拒时改用 Windows 10 起的 DuplicateOutput1，两者都失败才退回 GDI。
        ComPtr<IDXGIOutput5> output5;
        const DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
        const HRESULT second = SUCCEEDED(selectedOutput.As(&output5))
            ? output5->DuplicateOutput1(device_.Get(), 0, 1, formats, &duplication_) : E_NOINTERFACE;
        if (FAILED(second)) {
            if (log) {
                LogLine("启动桌面复制失败：0x%08X，DuplicateOutput1：0x%08X", static_cast<unsigned>(result), static_cast<unsigned>(second));
                if (result == E_ACCESSDENIED) LogDuplicationDenied();
            }
            return false;
        }
        if (log) LogLine("旧桌面复制接口被拒（0x%08X），已改用 DuplicateOutput1", static_cast<unsigned>(result));
    }
    DXGI_OUTDUPL_DESC desc{};
    duplication_->GetDesc(&desc);
    width_ = static_cast<int>(desc.ModeDesc.Width);
    height_ = static_cast<int>(desc.ModeDesc.Height);
    if (desc.ModeDesc.RefreshRate.Denominator) refreshHz_ = static_cast<int>(
        static_cast<double>(desc.ModeDesc.RefreshRate.Numerator) / desc.ModeDesc.RefreshRate.Denominator + 0.5);
    DEVMODEW mode{};
    mode.dmSize = sizeof mode;
    if (EnumDisplaySettingsW(outputDesc.DeviceName, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1)
        refreshHz_ = static_cast<int>(mode.dmDisplayFrequency);
    DXGI_ADAPTER_DESC1 adapterDesc{};
    selectedAdapter->GetDesc1(&adapterDesc);
    adapterName_ = adapterDesc.Description;
    D3D11_TEXTURE2D_DESC image{};
    image.Width = desc.ModeDesc.Width;
    image.Height = desc.ModeDesc.Height;
    image.MipLevels = image.ArraySize = 1;
    image.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    image.SampleDesc.Count = 1;
    image.Usage = D3D11_USAGE_DEFAULT;
    image.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    result = device_->CreateTexture2D(&image, nullptr, &image_);
    if (FAILED(result)) return false;
    if (log) LogLine("GPU 主屏采集：%s，%dx%d，屏幕刷新率 %d Hz", WideToUtf8(adapterName_).c_str(), width_, height_, refreshHz_);
    return true;
}

CaptureResult GpuCapture::Read(uint64_t& timestamp) {
    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> resource;
    const HRESULT result = duplication_->AcquireNextFrame(0, &info, &resource);
    if (result == DXGI_ERROR_WAIT_TIMEOUT) return CaptureResult::Idle;
    if (FAILED(result)) {
        LogLine("GPU 采集失效，需要重连：0x%08X", static_cast<unsigned>(result));
        return CaptureResult::Failed;
    }
    ComPtr<ID3D11Texture2D> frame;
    const bool valid = resource && SUCCEEDED(resource.As(&frame));
    // 仅鼠标形状或位置变化时不重复编码整张静止桌面。
    const bool updated = info.LastPresentTime.QuadPart != 0 || !haveFrame_;
    if (valid && updated) {
        context_->CopyResource(image_.Get(), frame.Get());
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        const auto ticks = info.LastPresentTime.QuadPart;
        timestamp = ticks > 0 ? static_cast<uint64_t>(ticks / frequency.QuadPart * 1000000 +
                    ticks % frequency.QuadPart * 1000000 / frequency.QuadPart) : NowMicros();
        haveFrame_ = true;
    }
    const HRESULT release = duplication_->ReleaseFrame();
    if (!valid || FAILED(release)) return CaptureResult::Failed;
    return updated ? CaptureResult::Frame : CaptureResult::Idle;
}
