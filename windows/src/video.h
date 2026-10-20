#pragma once

#include "common.h"

#include <condition_variable>
#include <d3d11_1.h>
#include <deque>
#include <dxgi1_6.h>
#include <functional>
#include <map>
#include <mfapi.h>
#include <mftransform.h>
#include <thread>

struct EncodedFrame {
    std::vector<uint8_t> data;
    uint64_t captureMicros = 0;  // Windows 时钟
    uint64_t enqueueMicros = 0;
    bool keyframe = false;
    bool refine = false;  // 静止画面的补清晰帧：不计入帧率与延迟
    // 本帧变化的图块（有则让这些块退回视频显示）
    bool hasDirty = false;
    uint32_t dirtySerial = 0;
    int tileSize = 0, tilesX = 0, tilesY = 0;
    std::vector<uint8_t> dirtyBits;
};

struct VideoStats {
    uint32_t presented = 0;
    uint32_t decoded = 0;
    uint64_t latencySumMicros = 0;
    uint32_t latencySamples = 0;
    uint32_t losslessTiles = 0;
    uint64_t decodeMicros = 0, renderMicros = 0, queueMicros = 0;
    uint32_t decodeBatches = 0, renderSamples = 0, queueSamples = 0, maxQueue = 0, decodeErrors = 0;
};

// 硬件解码（Media Foundation H.264，低延迟模式）+ D3D11 视频处理器 + 无损图块覆盖层 + 翻转模型交换链。
// 解码、无损图块上传和渲染都在专用线程上按到达顺序处理；每批只显示最新一帧，不排队。
class VideoPipeline {
public:
    bool Initialize(HWND window);
    void Shutdown();

    void Submit(EncodedFrame&& frame);
    void SubmitLossless(std::vector<uint8_t>&& payload);
    void RequestResize();
    void RequestRedraw();
    void ResetOverlay();

    // 当前视频帧尺寸（用于鼠标坐标映射），未收到画面时为 0。
    int FrameWidth() const { return frameWidth_.load(); }
    int FrameHeight() const { return frameHeight_.load(); }

    VideoStats TakeStats();
    std::function<void()> onDecodeError;         // 需要请求关键帧时回调（视频线程）
    std::atomic<bool> overlayEnabled{true};     // Ctrl+Alt+L 切换，用于对比无损层与视频
    std::atomic<int64_t> clockOffsetMicros{0};  // Windows 时钟 − 本机时钟
    std::atomic<bool> clockOffsetValid{false};

private:
    struct Item {
        bool lossless = false;
        EncodedFrame frame;
        std::vector<uint8_t> payload;
    };

    void Run();
    bool CreateDecoder();
    bool SetOutputType();
    bool CreateOverlayPipeline();
    bool Decode(const EncodedFrame& frame, ComPtr<IMFSample>& latest);
    bool DrainOutput(ComPtr<IMFSample>& latest);
    void ApplyDirty(const EncodedFrame& frame);
    void ApplyLossless(const std::vector<uint8_t>& payload);
    void ResetOverlayState(int width, int height, int tileSize);
    void EnsureGrid(int tilesX, int tilesY, int tileSize);
    void EnsureTextures(int width, int height);
    void ResizeSwapChain();
    bool EnsureProcessor(UINT width, UINT height);
    bool Render(IMFSample* sample);
    void DrawOverlay(const Rect& dest, int frameW, int frameH);
    void ClearBackground();

    HWND window_ = nullptr;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11VideoDevice> videoDevice_;
    ComPtr<ID3D11VideoContext> videoContext_;
    ComPtr<IDXGISwapChain1> swapChain_;
    bool tearing_ = false;

    ComPtr<IMFDXGIDeviceManager> deviceManager_;
    UINT resetToken_ = 0;
    ComPtr<IMFTransform> decoder_;
    RECT displayAperture_{};

    ComPtr<ID3D11VideoProcessorEnumerator> processorEnum_;
    ComPtr<ID3D11VideoProcessor> processor_;
    ComPtr<ID3D11VideoProcessorOutputView> outputView_;
    ComPtr<ID3D11RenderTargetView> backBufferView_;
    std::map<std::pair<ID3D11Texture2D*, UINT>, ComPtr<ID3D11VideoProcessorInputView>> inputViews_;
    UINT processorWidth_ = 0, processorHeight_ = 0;
    UINT backWidth_ = 0, backHeight_ = 0;
    ComPtr<IMFSample> lastSample_;

    // 无损覆盖层：整帧 BGRA 纹理 + 每个图块是否有效（R8 掩码纹理）。
    ComPtr<ID3D11VertexShader> overlayVS_;
    ComPtr<ID3D11PixelShader> overlayPS_;
    ComPtr<ID3D11Buffer> overlayParams_;
    ComPtr<ID3D11SamplerState> pointSampler_;
    ComPtr<ID3D11SamplerState> linearSampler_;
    ComPtr<ID3D11BlendState> opaqueBlend_;
    ComPtr<ID3D11RasterizerState> rasterizer_;
    ComPtr<ID3D11Texture2D> overlayTexture_;
    ComPtr<ID3D11ShaderResourceView> overlayView_;
    ComPtr<ID3D11Texture2D> maskTexture_;
    ComPtr<ID3D11ShaderResourceView> maskView_;
    int overlayWidth_ = 0, overlayHeight_ = 0, overlayTile_ = 0, tilesX_ = 0, tilesY_ = 0;
    std::vector<uint8_t> tileValid_;
    std::vector<uint32_t> tileDirtySerial_;
    bool maskChanged_ = false;
    int validTiles_ = 0;
    std::vector<uint8_t> tileBuffer_;
    std::vector<size_t> pendingTiles_;  // 本轮已上传、等 final 批到齐再生效的图块
    uint32_t pendingSerial_ = 0;
    bool fullRange_ = true;             // 视频 YUV 范围（Windows 端按全范围编码）
    bool processorFullRange_ = true;

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Item> queue_;
    bool resizePending_ = false;
    bool redrawPending_ = false;
    bool overlayResetPending_ = false;
    bool running_ = false;

    // 诊断统计（视频线程），有画面时每 2 秒写一次日志。
    uint64_t diagSince_ = 0;
    uint32_t diagFrames_ = 0, diagPresents_ = 0, diagBatches_ = 0, diagMaxBatch_ = 0;
    uint64_t diagDecodeMicros_ = 0, diagRenderMicros_ = 0;

    std::atomic<int> frameWidth_{0};
    std::atomic<int> frameHeight_{0};
    std::mutex statsMutex_;
    VideoStats stats_;
};
