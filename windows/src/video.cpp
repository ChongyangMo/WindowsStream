#include "video.h"

#include <codecapi.h>
#include <d3dcompiler.h>
#include <mferror.h>
#include <wmcodecdsp.h>

namespace {

bool Check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        LogLine("%s 失败 hr=0x%08X", what, static_cast<unsigned>(hr));
        return false;
    }
    return true;
}

// LZ4 块格式解码（与 Apple Compression 的 COMPRESSION_LZ4_RAW 对应）。输出必须恰好填满 dst。
bool Lz4Decompress(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstSize) {
    const uint8_t* ip = src;
    const uint8_t* const iend = src + srcSize;
    uint8_t* op = dst;
    uint8_t* const oend = dst + dstSize;
    while (ip < iend) {
        unsigned token = *ip++;
        size_t literals = token >> 4;
        if (literals == 15) {
            unsigned b;
            do {
                if (ip >= iend) return false;
                b = *ip++;
                literals += b;
            } while (b == 255);
        }
        if (literals > static_cast<size_t>(iend - ip) || literals > static_cast<size_t>(oend - op)) return false;
        memcpy(op, ip, literals);
        op += literals;
        ip += literals;
        if (ip >= iend) break;  // 最后一个序列只有字面量
        if (iend - ip < 2) return false;
        size_t offset = static_cast<size_t>(ip[0]) | static_cast<size_t>(ip[1]) << 8;
        ip += 2;
        if (offset == 0 || offset > static_cast<size_t>(op - dst)) return false;
        size_t match = token & 15;
        if (match == 15) {
            unsigned b;
            do {
                if (ip >= iend) return false;
                b = *ip++;
                match += b;
            } while (b == 255);
        }
        match += 4;
        if (match > static_cast<size_t>(oend - op)) return false;
        const uint8_t* from = op - offset;
        for (size_t i = 0; i < match; ++i) op[i] = from[i];  // 允许重叠
        op += match;
    }
    return op == oend;
}

// 覆盖层着色器：按图块掩码只绘制有效的无损像素，其余保留下面的视频。
const char kOverlayShader[] = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VS(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2(id & 1, id >> 1);
    o.uv = uv;
    o.pos = float4(uv.x * 2 - 1, 1 - uv.y * 2, 0, 1);
    return o;
}
Texture2D<float4> Image : register(t0);
Texture2D<float> Mask : register(t1);
SamplerState Smp : register(s0);
cbuffer Params : register(b0) { float2 ImageSize; float TileSize; float Unused; };
float4 PS(VSOut i) : SV_Target {
    int2 texel = int2(min(i.uv * ImageSize, ImageSize - 1));
    if (Mask.Load(int3(texel / (int)TileSize, 0)) < 0.5) discard;
    return Image.Sample(Smp, i.uv);
}
)";

}  // namespace

bool VideoPipeline::Initialize(HWND window) {
    window_ = window;
    if (!Check(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET), "MFStartup")) return false;

    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    if (!Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2, D3D11_SDK_VERSION,
                                 &device_, nullptr, &context_),
               "D3D11CreateDevice")) {
        return false;
    }
    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(device_.As(&multithread))) {
        multithread->SetMultithreadProtected(TRUE);
    }
    if (!Check(device_.As(&videoDevice_), "ID3D11VideoDevice") || !Check(context_.As(&videoContext_), "ID3D11VideoContext")) {
        return false;
    }

    ComPtr<IDXGIDevice1> dxgiDevice;
    device_.As(&dxgiDevice);
    dxgiDevice->SetMaximumFrameLatency(1);
    ComPtr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(&adapter);
    DXGI_ADAPTER_DESC adapterDesc{};
    adapter->GetDesc(&adapterDesc);
    LogLine("显卡：%s", WideToUtf8(adapterDesc.Description).c_str());
    ComPtr<IDXGIFactory2> factory;
    adapter->GetParent(IID_PPV_ARGS(&factory));
    ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(factory.As(&factory5))) {
        BOOL allow = FALSE;
        if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof allow))) {
            tearing_ = allow != FALSE;
        }
    }

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 3;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Scaling = DXGI_SCALING_NONE;
    desc.Flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    if (!Check(factory->CreateSwapChainForHwnd(device_.Get(), window_, &desc, nullptr, nullptr, &swapChain_),
               "CreateSwapChainForHwnd")) {
        return false;
    }
    factory->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER);
    swapChain_->GetDesc1(&desc);
    backWidth_ = desc.Width;
    backHeight_ = desc.Height;

    if (!Check(MFCreateDXGIDeviceManager(&resetToken_, &deviceManager_), "MFCreateDXGIDeviceManager") ||
        !Check(deviceManager_->ResetDevice(device_.Get(), resetToken_), "ResetDevice") || !CreateDecoder() ||
        !CreateOverlayPipeline()) {
        return false;
    }
    LogLine("视频管线就绪，撕裂模式 %s", tearing_ ? "可用" : "不可用");

    running_ = true;
    thread_ = std::thread([this] { Run(); });
    return true;
}

void VideoPipeline::Shutdown() {
    {
        std::lock_guard lock(mutex_);
        running_ = false;
    }
    wake_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool VideoPipeline::CreateDecoder() {
    if (!Check(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&decoder_)),
               "创建 H.264 解码器")) {
        return false;
    }
    ComPtr<IMFAttributes> attributes;
    decoder_->GetAttributes(&attributes);
    UINT32 d3dAware = FALSE;
    attributes->GetUINT32(MF_SA_D3D11_AWARE, &d3dAware);
    attributes->SetUINT32(CODECAPI_AVDecVideoAcceleration_H264, TRUE);
    attributes->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);
    ComPtr<ICodecAPI> codecApi;
    if (SUCCEEDED(decoder_.As(&codecApi))) {
        VARIANT value{};
        value.vt = VT_UI4;
        value.ulVal = TRUE;
        codecApi->SetValue(&CODECAPI_AVLowLatencyMode, &value);
    }
    if (!d3dAware) {
        LogLine("H.264 解码器不支持 D3D11，无法使用硬件解码");
        return false;
    }
    if (!Check(decoder_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(deviceManager_.Get())),
               "设置 D3D 管理器")) {
        return false;
    }

    ComPtr<IMFMediaType> input;
    MFCreateMediaType(&input);
    input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, 1920, 1080);
    MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, 60, 1);
    if (!Check(decoder_->SetInputType(0, input.Get(), 0), "设置解码输入格式") || !SetOutputType()) {
        return false;
    }
    decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

bool VideoPipeline::SetOutputType() {
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> type;
        HRESULT hr = decoder_->GetOutputAvailableType(0, i, &type);
        if (FAILED(hr)) {
            LogLine("解码器没有 NV12 输出格式");
            return false;
        }
        GUID subtype{};
        type->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (subtype != MFVideoFormat_NV12) {
            continue;
        }
        if (!Check(decoder_->SetOutputType(0, type.Get(), 0), "设置解码输出格式")) {
            return false;
        }
        UINT32 width = 0, height = 0;
        MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height);
        MFVideoArea area{};
        if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof area, nullptr))) {
            displayAperture_ = {area.OffsetX.value, area.OffsetY.value, area.OffsetX.value + static_cast<LONG>(area.Area.cx),
                                area.OffsetY.value + static_cast<LONG>(area.Area.cy)};
        } else {
            displayAperture_ = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        }
        frameWidth_ = displayAperture_.right - displayAperture_.left;
        frameHeight_ = displayAperture_.bottom - displayAperture_.top;
        UINT32 range = MFNominalRange_16_235;
        type->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &range);
        fullRange_ = range == MFNominalRange_0_255;  // Windows 被控端默认输出 BT.709 有限范围
        LogLine("解码输出 %ux%u，显示区域 %dx%d，%s", width, height, frameWidth_.load(), frameHeight_.load(),
                fullRange_ ? "全范围" : "有限范围");
        return true;
    }
}

bool VideoPipeline::CreateOverlayPipeline() {
    ComPtr<ID3DBlob> vs, ps, errors;
    if (FAILED(D3DCompile(kOverlayShader, sizeof kOverlayShader - 1, "overlay", nullptr, nullptr, "VS", "vs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &errors)) ||
        FAILED(D3DCompile(kOverlayShader, sizeof kOverlayShader - 1, "overlay", nullptr, nullptr, "PS", "ps_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &errors))) {
        LogLine("编译覆盖层着色器失败：%s", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        return false;
    }
    if (!Check(device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &overlayVS_), "CreateVertexShader") ||
        !Check(device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &overlayPS_), "CreatePixelShader")) {
        return false;
    }

    D3D11_BUFFER_DESC params{};
    params.ByteWidth = 16;
    params.Usage = D3D11_USAGE_DEFAULT;
    params.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SAMPLER_DESC sampler{};
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    D3D11_SAMPLER_DESC linear = sampler;
    linear.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    return Check(device_->CreateBuffer(&params, nullptr, &overlayParams_), "CreateBuffer") &&
           Check(device_->CreateSamplerState(&sampler, &pointSampler_), "CreateSamplerState") &&
           Check(device_->CreateSamplerState(&linear, &linearSampler_), "CreateSamplerState") &&
           Check(device_->CreateRasterizerState(&raster, &rasterizer_), "CreateRasterizerState");
}

void VideoPipeline::Submit(EncodedFrame&& frame) {
    frame.enqueueMicros = NowMicros();
    {
        std::lock_guard lock(mutex_);
        Item item;
        item.frame = std::move(frame);
        queue_.push_back(std::move(item));
        std::lock_guard statsLock(statsMutex_);
        stats_.maxQueue = std::max(stats_.maxQueue, static_cast<uint32_t>(queue_.size()));
    }
    wake_.notify_one();
}

void VideoPipeline::SubmitLossless(std::vector<uint8_t>&& payload) {
    {
        std::lock_guard lock(mutex_);
        Item item;
        item.lossless = true;
        item.payload = std::move(payload);
        queue_.push_back(std::move(item));
    }
    wake_.notify_one();
}

void VideoPipeline::RequestResize() {
    {
        std::lock_guard lock(mutex_);
        resizePending_ = true;
    }
    wake_.notify_one();
}

void VideoPipeline::RequestRedraw() {
    {
        std::lock_guard lock(mutex_);
        redrawPending_ = true;
    }
    wake_.notify_one();
}

void VideoPipeline::ResetOverlay() {
    {
        std::lock_guard lock(mutex_);
        overlayResetPending_ = true;
    }
    wake_.notify_one();
}

VideoStats VideoPipeline::TakeStats() {
    std::lock_guard lock(statsMutex_);
    VideoStats result = stats_;
    stats_ = {};
    return result;
}

void VideoPipeline::Run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ClearBackground();
    for (;;) {
        std::deque<Item> items;
        bool resize = false, redraw = false, resetOverlay = false;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] {
                return !running_ || !queue_.empty() || resizePending_ || redrawPending_ || overlayResetPending_;
            });
            if (!running_) break;
            items.swap(queue_);
            resize = std::exchange(resizePending_, false);
            redraw = std::exchange(redrawPending_, false);
            resetOverlay = std::exchange(overlayResetPending_, false);
        }
        if (resetOverlay) {
            ResetOverlayState(0, 0, 0);
        }
        if (resize) {
            ResizeSwapChain();
        }

        // 按到达顺序处理：先让变化的图块退回视频，再解码；无损图块到达后覆盖。
        ComPtr<IMFSample> latest;
        const EncodedFrame* newest = nullptr;
        bool overlayChanged = false;
        uint32_t videoItems = 0;
        uint32_t queueSamples = 0, decodeErrors = 0;
        uint64_t queueMicros = 0;
        uint64_t decodeStart = NowMicros();
        for (auto& item : items) {
            if (item.lossless) {
                ApplyLossless(item.payload);
                overlayChanged = true;
                continue;
            }
            ApplyDirty(item.frame);
            if (item.frame.enqueueMicros && decodeStart >= item.frame.enqueueMicros) {
                queueMicros += decodeStart - item.frame.enqueueMicros;
                ++queueSamples;
            }
            if (!Decode(item.frame, latest)) {
                ++decodeErrors;
                if (onDecodeError) onDecodeError();
            }
            newest = &item.frame;
            ++videoItems;
        }
        uint64_t decodeEnd = NowMicros();
        if (videoItems) {
            std::lock_guard lock(statsMutex_);
            stats_.decodeMicros += decodeEnd - decodeStart;
            ++stats_.decodeBatches;
            stats_.queueMicros += queueMicros;
            stats_.queueSamples += queueSamples;
            stats_.decodeErrors += decodeErrors;
        }

        if (latest) {
            lastSample_ = latest;
            const bool presented = Render(lastSample_.Get());
            uint64_t renderEnd = NowMicros();
            if (newest && !newest->refine) {
                diagFrames_ += videoItems;
                if (presented) diagPresents_++;
                diagBatches_++;
                diagMaxBatch_ = std::max(diagMaxBatch_, videoItems);
                diagDecodeMicros_ += decodeEnd - decodeStart;
                diagRenderMicros_ += renderEnd - decodeEnd;
            }
            if (diagSince_ == 0) diagSince_ = renderEnd;
            if (renderEnd - diagSince_ >= 2'000'000) {
                if (diagPresents_ > 0) {
                    double seconds = (renderEnd - diagSince_) / 1e6;
                    LogLine("性能 收到 %.1f fps，显示 %.1f fps，单批最多 %u 帧，解码 %.2f ms/批，渲染 %.2f ms", diagFrames_ / seconds,
                            diagPresents_ / seconds, diagMaxBatch_, diagDecodeMicros_ / 1000.0 / diagBatches_,
                            diagRenderMicros_ / 1000.0 / diagPresents_);
                }
                diagSince_ = renderEnd;
                diagFrames_ = diagPresents_ = diagBatches_ = diagMaxBatch_ = 0;
                diagDecodeMicros_ = diagRenderMicros_ = 0;
            }
            if (newest && !newest->refine && presented) {
                std::lock_guard lock(statsMutex_);
                stats_.presented++;
                stats_.renderMicros += renderEnd - decodeEnd;
                ++stats_.renderSamples;
            }
            LONGLONG sampleTime = 0;
            if (newest && !newest->refine && presented && clockOffsetValid && SUCCEEDED(latest->GetSampleTime(&sampleTime))) {
                int64_t captureLocal = sampleTime / 10 - clockOffsetMicros.load();
                int64_t latency = static_cast<int64_t>(NowMicros()) - captureLocal;
                if (latency > 0 && latency < 2'000'000) {
                    std::lock_guard lock(statsMutex_);
                    stats_.latencySumMicros += static_cast<uint64_t>(latency);
                    stats_.latencySamples++;
                }
            }
        } else if ((resize || redraw || overlayChanged || resetOverlay) && lastSample_) {
            Render(lastSample_.Get());
        } else if (resize || redraw) {
            ClearBackground();
        }
    }
    lastSample_.Reset();
    CoUninitialize();
}

bool VideoPipeline::Decode(const EncodedFrame& frame, ComPtr<IMFSample>& latest) {
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(frame.data.size()), &buffer))) return false;
    BYTE* p = nullptr;
    buffer->Lock(&p, nullptr, nullptr);
    memcpy(p, frame.data.data(), frame.data.size());
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(frame.data.size()));

    ComPtr<IMFSample> sample;
    MFCreateSample(&sample);
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(static_cast<LONGLONG>(frame.captureMicros) * 10);
    if (frame.keyframe) {
        sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
    }

    HRESULT hr = decoder_->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        DrainOutput(latest);
        hr = decoder_->ProcessInput(0, sample.Get(), 0);
    }
    if (FAILED(hr)) {
        LogLine("ProcessInput 失败 hr=0x%08X", static_cast<unsigned>(hr));
        return false;
    }
    return DrainOutput(latest);
}

bool VideoPipeline::DrainOutput(ComPtr<IMFSample>& latest) {
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER output{};
        DWORD status = 0;
        HRESULT hr = decoder_->ProcessOutput(0, 1, &output, &status);
        if (output.pEvents) {
            output.pEvents->Release();
        }
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            return true;
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (!SetOutputType()) return false;
            continue;
        }
        if (FAILED(hr)) {
            LogLine("ProcessOutput 失败 hr=0x%08X", static_cast<unsigned>(hr));
            return false;
        }
        latest.Attach(output.pSample);
        std::lock_guard lock(statsMutex_);
        stats_.decoded++;
    }
}

// ---------------- 无损覆盖层 ----------------

void VideoPipeline::ResetOverlayState(int width, int height, int tileSize) {
    overlayTexture_.Reset();
    overlayView_.Reset();
    maskTexture_.Reset();
    maskView_.Reset();
    overlayWidth_ = overlayHeight_ = 0;
    overlayTile_ = tilesX_ = tilesY_ = 0;
    tileValid_.clear();
    tileDirtySerial_.clear();
    validTiles_ = 0;
    maskChanged_ = false;
    if (tileSize > 0 && width > 0 && height > 0) {
        EnsureGrid((width + tileSize - 1) / tileSize, (height + tileSize - 1) / tileSize, tileSize);
        EnsureTextures(width, height);
    }
}

// 图块网格（有效标记 + 最近变化序号）独立于纹理维护：无损数据到达前的变化信息也要记住。
void VideoPipeline::EnsureGrid(int tilesX, int tilesY, int tileSize) {
    if (tilesX == tilesX_ && tilesY == tilesY_ && tileSize == overlayTile_) return;
    overlayTexture_.Reset();
    overlayView_.Reset();
    maskTexture_.Reset();
    maskView_.Reset();
    overlayWidth_ = overlayHeight_ = 0;
    overlayTile_ = tileSize;
    tilesX_ = tilesX;
    tilesY_ = tilesY;
    tileValid_.assign(static_cast<size_t>(tilesX) * tilesY, 0);
    tileDirtySerial_.assign(static_cast<size_t>(tilesX) * tilesY, 0);
    pendingTiles_.clear();
    validTiles_ = 0;
    maskChanged_ = true;
}

void VideoPipeline::EnsureTextures(int width, int height) {
    if (overlayTexture_ && width == overlayWidth_ && height == overlayHeight_) return;
    overlayTexture_.Reset();
    overlayView_.Reset();
    maskTexture_.Reset();
    maskView_.Reset();
    overlayWidth_ = overlayHeight_ = 0;
    std::fill(tileValid_.begin(), tileValid_.end(), uint8_t{0});
    pendingTiles_.clear();
    validTiles_ = 0;
    maskChanged_ = true;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_TEXTURE2D_DESC maskDesc = desc;
    maskDesc.Width = static_cast<UINT>(tilesX_);
    maskDesc.Height = static_cast<UINT>(tilesY_);
    maskDesc.Format = DXGI_FORMAT_R8_UNORM;
    if (!Check(device_->CreateTexture2D(&desc, nullptr, &overlayTexture_), "创建无损纹理") ||
        !Check(device_->CreateShaderResourceView(overlayTexture_.Get(), nullptr, &overlayView_), "无损纹理视图") ||
        !Check(device_->CreateTexture2D(&maskDesc, nullptr, &maskTexture_), "创建掩码纹理") ||
        !Check(device_->CreateShaderResourceView(maskTexture_.Get(), nullptr, &maskView_), "掩码纹理视图")) {
        overlayTexture_.Reset();
        overlayView_.Reset();
        maskTexture_.Reset();
        maskView_.Reset();
        return;
    }
    overlayWidth_ = width;
    overlayHeight_ = height;
}

void VideoPipeline::ApplyDirty(const EncodedFrame& frame) {
    if (!frame.hasDirty || frame.tileSize <= 0 || frame.tilesX <= 0 || frame.tilesY <= 0) return;
    EnsureGrid(frame.tilesX, frame.tilesY, frame.tileSize);  // 网格变化（画面尺寸变了）时旧无损层作废
    size_t count = static_cast<size_t>(tilesX_) * tilesY_;
    for (size_t i = 0; i < count && (i >> 3) < frame.dirtyBits.size(); ++i) {
        if (frame.dirtyBits[i >> 3] & (1u << (i & 7))) {
            if (tileValid_[i]) {
                tileValid_[i] = 0;
                --validTiles_;
                maskChanged_ = true;
            }
            if (static_cast<int32_t>(frame.dirtySerial - tileDirtySerial_[i]) > 0) {
                tileDirtySerial_[i] = frame.dirtySerial;
            }
        }
    }
}

// 一轮无损更新分多批到达：每批先上传纹理并暂存，收到 final 批才让整轮一起生效（避免一批批出现的阶梯）。
// 生效时再检查一次序号：这期间又变过的图块不生效，继续显示视频。
void VideoPipeline::ApplyLossless(const std::vector<uint8_t>& payload) {
    if (payload.size() < 13) return;
    const uint8_t* p = payload.data();
    const uint8_t* end = p + payload.size();
    uint32_t serial = ReadU32(p);
    int width = ReadU16(p + 4), height = ReadU16(p + 6), tile = ReadU16(p + 8), count = ReadU16(p + 10);
    bool final = (p[12] & 1) != 0;
    p += 13;
    if (width <= 0 || height <= 0 || tile <= 0) return;
    EnsureGrid((width + tile - 1) / tile, (height + tile - 1) / tile, tile);
    EnsureTextures(width, height);
    if (!overlayTexture_) return;
    if (serial != pendingSerial_) {
        pendingTiles_.clear();  // 上一轮被中止了，丢弃
        pendingSerial_ = serial;
    }

    tileBuffer_.resize(static_cast<size_t>(tile) * tile * 4);
    for (int n = 0; n < count; ++n) {
        if (end - p < 8) break;
        int tx = ReadU16(p), ty = ReadU16(p + 2);
        uint32_t lengthField = ReadU32(p + 4);
        p += 8;
        bool raw = (lengthField & 0x8000'0000u) != 0;
        size_t length = lengthField & 0x7FFF'FFFFu;
        if (length > static_cast<size_t>(end - p) || tx >= tilesX_ || ty >= tilesY_) break;
        int w = std::min(tile, width - tx * tile);
        int h = std::min(tile, height - ty * tile);
        size_t rawSize = static_cast<size_t>(w) * h * 4;
        bool ok = raw ? (length == rawSize) : Lz4Decompress(p, length, tileBuffer_.data(), rawSize);
        const uint8_t* pixels = raw ? p : tileBuffer_.data();
        p += length;
        size_t i = static_cast<size_t>(ty) * tilesX_ + tx;
        // 这块在无损数据生成之后又变过：数据已过期，丢弃。
        if (!ok || static_cast<int32_t>(serial - tileDirtySerial_[i]) < 0) continue;
        D3D11_BOX box{static_cast<UINT>(tx * tile), static_cast<UINT>(ty * tile), 0,
                      static_cast<UINT>(tx * tile + w), static_cast<UINT>(ty * tile + h), 1};
        context_->UpdateSubresource(overlayTexture_.Get(), 0, &box, pixels, static_cast<UINT>(w * 4), 0);
        pendingTiles_.push_back(i);
    }
    if (!final) return;

    int applied = 0;
    for (size_t i : pendingTiles_) {
        if (static_cast<int32_t>(serial - tileDirtySerial_[i]) < 0) continue;
        if (!tileValid_[i]) {
            tileValid_[i] = 1;
            ++validTiles_;
            maskChanged_ = true;
        }
        ++applied;
    }
    pendingTiles_.clear();
    std::lock_guard lock(statsMutex_);
    stats_.losslessTiles += static_cast<uint32_t>(applied);
}

void VideoPipeline::DrawOverlay(const Rect& dest, int frameW, int frameH) {
    if (!overlayEnabled || !overlayView_ || validTiles_ <= 0 || overlayWidth_ != frameW || overlayHeight_ != frameH || !backBufferView_) {
        return;
    }
    if (maskChanged_) {
        std::vector<uint8_t> mask(tileValid_.size());
        for (size_t i = 0; i < mask.size(); ++i) mask[i] = tileValid_[i] ? 255 : 0;
        context_->UpdateSubresource(maskTexture_.Get(), 0, nullptr, mask.data(), static_cast<UINT>(tilesX_), 0);
        maskChanged_ = false;
    }
    float params[4] = {static_cast<float>(overlayWidth_), static_cast<float>(overlayHeight_), static_cast<float>(overlayTile_), 0};
    context_->UpdateSubresource(overlayParams_.Get(), 0, nullptr, params, 0, 0);

    D3D11_VIEWPORT viewport{static_cast<float>(dest.x), static_cast<float>(dest.y), static_cast<float>(dest.w),
                            static_cast<float>(dest.h), 0.0f, 1.0f};
    ID3D11RenderTargetView* targets[] = {backBufferView_.Get()};
    ID3D11ShaderResourceView* views[] = {overlayView_.Get(), maskView_.Get()};
    bool exact = dest.w == frameW && dest.h == frameH;
    ID3D11SamplerState* sampler = exact ? pointSampler_.Get() : linearSampler_.Get();
    context_->OMSetRenderTargets(1, targets, nullptr);
    context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    context_->RSSetState(rasterizer_.Get());
    context_->RSSetViewports(1, &viewport);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context_->VSSetShader(overlayVS_.Get(), nullptr, 0);
    context_->PSSetShader(overlayPS_.Get(), nullptr, 0);
    context_->PSSetShaderResources(0, 2, views);
    context_->PSSetSamplers(0, 1, &sampler);
    ID3D11Buffer* buffers[] = {overlayParams_.Get()};
    context_->PSSetConstantBuffers(0, 1, buffers);
    context_->Draw(4, 0);
    ID3D11ShaderResourceView* none[2] = {};
    context_->PSSetShaderResources(0, 2, none);
}

// ---------------- 渲染 ----------------

void VideoPipeline::ResizeSwapChain() {
    RECT client{};
    GetClientRect(window_, &client);
    UINT width = static_cast<UINT>(std::max<LONG>(client.right - client.left, 1));
    UINT height = static_cast<UINT>(std::max<LONG>(client.bottom - client.top, 1));
    if (width == backWidth_ && height == backHeight_) return;

    outputView_.Reset();
    backBufferView_.Reset();
    processor_.Reset();
    processorEnum_.Reset();
    inputViews_.clear();
    context_->ClearState();
    context_->Flush();
    HRESULT hr = swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN,
                                           tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    if (Check(hr, "ResizeBuffers")) {
        backWidth_ = width;
        backHeight_ = height;
    }
}

bool VideoPipeline::EnsureProcessor(UINT width, UINT height) {
    if (processor_ && outputView_ && backBufferView_ && width == processorWidth_ && height == processorHeight_ &&
        fullRange_ == processorFullRange_) {
        return true;
    }

    outputView_.Reset();
    backBufferView_.Reset();
    processor_.Reset();
    processorEnum_.Reset();
    inputViews_.clear();

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputWidth = width;
    content.InputHeight = height;
    content.OutputWidth = backWidth_;
    content.OutputHeight = backHeight_;
    content.InputFrameRate = {60, 1};
    content.OutputFrameRate = {60, 1};
    content.Usage = D3D11_VIDEO_USAGE_OPTIMAL_QUALITY;
    if (!Check(videoDevice_->CreateVideoProcessorEnumerator(&content, &processorEnum_), "CreateVideoProcessorEnumerator") ||
        !Check(videoDevice_->CreateVideoProcessor(processorEnum_.Get(), 0, &processor_), "CreateVideoProcessor")) {
        return false;
    }

    ComPtr<ID3D11Texture2D> backBuffer;
    swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc{};
    outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    if (!Check(videoDevice_->CreateVideoProcessorOutputView(backBuffer.Get(), processorEnum_.Get(), &outputDesc, &outputView_),
               "CreateVideoProcessorOutputView") ||
        !Check(device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &backBufferView_), "CreateRenderTargetView")) {
        return false;
    }

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputSpace{};
    inputSpace.YCbCr_Matrix = 1;  // BT.709
    inputSpace.Nominal_Range = fullRange_ ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255 : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    videoContext_->VideoProcessorSetStreamColorSpace(processor_.Get(), 0, &inputSpace);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputSpace{};
    outputSpace.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    videoContext_->VideoProcessorSetOutputColorSpace(processor_.Get(), &outputSpace);
    videoContext_->VideoProcessorSetStreamFrameFormat(processor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    videoContext_->VideoProcessorSetStreamAutoProcessingMode(processor_.Get(), 0, FALSE);
    D3D11_VIDEO_COLOR black{};
    black.RGBA = {0.0f, 0.0f, 0.0f, 1.0f};
    videoContext_->VideoProcessorSetOutputBackgroundColor(processor_.Get(), FALSE, &black);

    processorWidth_ = width;
    processorHeight_ = height;
    processorFullRange_ = fullRange_;
    return true;
}

bool VideoPipeline::Render(IMFSample* sample) {
    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMFDXGIBuffer> dxgiBuffer;
    ComPtr<ID3D11Texture2D> texture;
    UINT subresource = 0;
    if (FAILED(sample->GetBufferByIndex(0, &buffer)) || FAILED(buffer.As(&dxgiBuffer)) ||
        FAILED(dxgiBuffer->GetResource(IID_PPV_ARGS(&texture))) || FAILED(dxgiBuffer->GetSubresourceIndex(&subresource))) {
        return false;
    }
    D3D11_TEXTURE2D_DESC textureDesc{};
    texture->GetDesc(&textureDesc);
    if (!EnsureProcessor(textureDesc.Width, textureDesc.Height)) return false;

    auto key = std::make_pair(texture.Get(), subresource);
    auto found = inputViews_.find(key);
    if (found == inputViews_.end()) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc{};
        inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        inputDesc.Texture2D.ArraySlice = subresource;
        ComPtr<ID3D11VideoProcessorInputView> view;
        if (!Check(videoDevice_->CreateVideoProcessorInputView(texture.Get(), processorEnum_.Get(), &inputDesc, &view),
                   "CreateVideoProcessorInputView")) {
            return false;
        }
        found = inputViews_.emplace(key, view).first;
    }

    int frameW = displayAperture_.right - displayAperture_.left;
    int frameH = displayAperture_.bottom - displayAperture_.top;
    Rect fit = VideoRect(static_cast<int>(backWidth_), static_cast<int>(backHeight_), frameW, frameH);
    RECT source = displayAperture_;
    RECT dest = {fit.x, fit.y, fit.x + fit.w, fit.y + fit.h};
    RECT target = {0, 0, static_cast<LONG>(backWidth_), static_cast<LONG>(backHeight_)};
    videoContext_->VideoProcessorSetStreamSourceRect(processor_.Get(), 0, TRUE, &source);
    videoContext_->VideoProcessorSetStreamDestRect(processor_.Get(), 0, TRUE, &dest);
    videoContext_->VideoProcessorSetOutputTargetRect(processor_.Get(), TRUE, &target);

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = found->second.Get();
    if (!Check(videoContext_->VideoProcessorBlt(processor_.Get(), outputView_.Get(), 0, 1, &stream), "VideoProcessorBlt")) {
        return false;
    }
    DrawOverlay(fit, frameW, frameH);
    const auto result = swapChain_->Present(0, tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
    return SUCCEEDED(result) && result != DXGI_STATUS_OCCLUDED;
}

void VideoPipeline::ClearBackground() {
    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) return;
    ComPtr<ID3D11RenderTargetView> target;
    if (FAILED(device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &target))) return;
    const float color[4] = {0.07f, 0.07f, 0.08f, 1.0f};
    context_->ClearRenderTargetView(target.Get(), color);
    swapChain_->Present(0, tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
}
