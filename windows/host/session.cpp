#include "session.h"
#include "stream.h"
#include "clipboard.h"
#include "cursor.h"
#include "input.h"
#include "discovery.h"
#include "locate/locator.h"
#include "service/service.h"
#include <cstdio>
#include <thread>

namespace {
std::string AverageMicros(uint64_t elapsed, uint64_t samples) {
    if (!samples) return "无样本";
    char text[64]{};
    snprintf(text, sizeof text, "%.2f ms", elapsed / 1000.0 / samples);
    return text;
}

bool Serve(HostChannel& channel, FrameStream& stream, const HostConfig& config,
           HWND owner, std::atomic<bool>& running, bool& gpuRecovered) {
    CursorSender cursor;
    const auto description = stream.Description();
    LogLine("视频链路：%s", description.c_str());
    Writer info;
    info.append(description.data(), description.size());
    if (!channel.Send(MsgHostInfo, info)) return false;
    auto initialTelemetry = EncodeTelemetry(stream.Telemetry());
    if (!channel.Send(MsgTelemetry, initialTelemetry)) return false;
    ClipboardBridge clipboard(owner);
    std::atomic<bool> connected{true};
    std::atomic<bool> forceKeyframe{true};
    std::thread receiver([&] {
        InputInjector input;
        uint8_t type = 0;
        std::vector<uint8_t> payload;
        while (running && connected && channel.Read(type, payload)) {
            switch (type) {
                case MsgPing:
                    if (payload.size() == 8) {
                        Writer pong;
                        pong.u64(ReadU64(payload.data()));
                        pong.u64(NowMicros());
                        if (!channel.Send(MsgPong, pong)) connected = false;
                    }
                    break;
                case MsgRequestKeyframe:
                    if (payload.empty()) forceKeyframe = true;
                    break;
                case MsgSecureAttention:
                    if (payload.empty() && servermode::SecureAgent() && !servermode::RequestAttention())
                        LogLine("请求服务器安全注意序列失败：%lu", GetLastError());
                    break;
                case MsgClipboard:
                    if (config.clipboard && !clipboard.Apply(payload)) LogLine("写入文本剪贴板失败");
                    break;
                case MsgViewSize:
                case MsgMonitorMode:
                    // 首版按被控端真实主屏和配置输出，不修改分辨率或创建虚拟屏。
                    break;
                default:
                    input.Handle(type, payload);
                    break;
            }
        }
        input.ReleaseAll();
        connected = false;
        channel.Shutdown();
    });
    // 退回 GDI 期间后台探测 GPU 桌面复制，恢复后结束本次连接，由外层重建 GPU 链路。
    std::atomic<bool> gpuBack{false};
    std::thread prober;
    if (stream.GpuFallback()) prober = std::thread([&] {
        while (running && connected && !gpuBack) {
            for (int i = 0; i < 100 && running && connected; ++i) Sleep(100);
            if (running && connected && GpuCapture().Initialize(false)) gpuBack = true;
        }
    });
    stream.Start();
    StreamPacer pacer;
    uint64_t next = NowMicros();
    uint64_t lastClipboard = 0, lastCursor = 0, lastStats = next, frames = 0, bytes = 0, sendMicros = 0;
    const uint64_t interval = 1000000 / stream.Fps();
    bool videoFailed = false, networkFailed = false;
    VideoEncoder::Sink sink = [&](const uint8_t* data, size_t size, uint64_t timestamp, bool keyframe) {
        Writer video;
        video.u64(timestamp);
        video.u8(keyframe ? 1 : 0);
        video.append(data, size);
        const auto started = NowMicros();
        if (!channel.Send(MsgVideo, video)) { networkFailed = true; return false; }
        sendMicros += NowMicros() - started;
        ++frames; bytes += video.bytes.size() + 5;
        return true;
    };
    while (running && connected) {
        if (gpuBack) {
            gpuRecovered = true;
            LogLine("GPU 桌面复制已恢复，重建连接切回 GPU 采集");
            break;
        }
        if (!stream.Pump(sink)) { videoFailed = !networkFailed; break; }
        auto now = NowMicros();
        if (now >= next) {
            bool submitted = false;
            const bool request = forceKeyframe.exchange(false);
            if (!stream.Capture(request, submitted, sink)) {
                if (stream.CaptureLost()) LogLine("采集源中断（合盖、屏幕关闭或桌面切换），保留当前编码配置重新建立连接");
                videoFailed = !networkFailed && !stream.CaptureLost();
                break;
            }
            if (request && !submitted) forceKeyframe = true;
            // 落后时跳到最新采样时刻，不堆积过期桌面或补发一串历史帧。
            next += interval;
            if (next < NowMicros()) next = NowMicros() + interval;
        }
        if (now - lastCursor >= 20000) {
            lastCursor = now;
            if (!cursor.Poll(channel)) break;
        }
        if (config.clipboard && now - lastClipboard >= 200000) {
            lastClipboard = now;
            std::string text;
            if (clipboard.Read(text)) {
                Writer payload;
                payload.append(text.data(), text.size());
                if (!channel.Send(MsgClipboard, payload)) break;
            }
        }
        now = NowMicros();
        if (now - lastStats >= 2000000) {
            const double seconds = (now - lastStats) / 1e6;
            const auto metrics = stream.TakeMetrics();
            auto sample = stream.Telemetry();
            sample.sampleMillis = static_cast<uint32_t>((now - lastStats) / 1000);
            sample.captureMilliFps = static_cast<uint32_t>(metrics.captured * 1000 / seconds);
            sample.submitMilliFps = static_cast<uint32_t>(metrics.submitted * 1000 / seconds);
            sample.outputMilliFps = static_cast<uint32_t>(frames * 1000 / seconds);
            sample.kbps = static_cast<uint32_t>(bytes * 8 / seconds / 1000);
            auto average = [](uint64_t us, uint64_t count) { return count ? static_cast<uint32_t>(us / count) : kUnknownMetric; };
            sample.captureUs = average(metrics.captureMicros, metrics.captured);
            sample.submitUs = average(metrics.submitMicros, metrics.submitAttempts);
            sample.outputAgeUs = average(metrics.outputAgeMicros, metrics.outputAgeSamples);
            sample.sendUs = average(sendMicros, frames);
            sample.skipped = static_cast<uint32_t>(metrics.skipped);
            auto encodedTelemetry = EncodeTelemetry(sample);
            if (!channel.Send(MsgTelemetry, encodedTelemetry)) break;
            char message[1024]{};
            snprintf(message, sizeof message,
                "视频统计：采集 %.1f 帧每秒，提交 %.1f，输出 %.1f，%.1f Mbps；采集 %s，提交调用 %s，采集到编码输出 %s，发送 %s；背压跳过 %llu，待编码 %u",
                metrics.captured / seconds, metrics.submitted / seconds, frames / seconds, bytes * 8 / seconds / 1e6,
                AverageMicros(metrics.captureMicros, metrics.captured).c_str(),
                AverageMicros(metrics.submitMicros, metrics.submitAttempts).c_str(),
                AverageMicros(metrics.outputAgeMicros, metrics.outputAgeSamples).c_str(),
                AverageMicros(sendMicros, frames).c_str(),
                static_cast<unsigned long long>(metrics.skipped), stream.Pending());
            LogLine("%s", message);
            Writer telemetry;
            telemetry.append(message, strlen(message));
            if (!channel.Send(MsgHostInfo, telemetry)) break;
            frames = bytes = sendMicros = 0;
            lastStats = now;
        }
        pacer.Wait(next > now ? next - now : 100);
    }
    connected = false;
    channel.Shutdown();
    receiver.join();
    if (prober.joinable()) prober.join();
    return videoFailed;
}
}

void HostServer::Stop() {
    running_ = false;
    std::lock_guard lock(mutex_);
    if (channel_) channel_->Shutdown();
}

void HostServer::Run(const HostConfig& config, HWND owner, const std::function<void(const std::wstring&)>& status) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const HRESULT media = SUCCEEDED(com) ? MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET) : com;
    if (FAILED(media)) {
        LogLine("初始化媒体服务失败：0x%08X", static_cast<unsigned>(media));
        status(L"初始化失败");
        if (SUCCEEDED(com)) CoUninitialize();
        return;
    }
    {
        DiscoveryResponder discovery;
        discovery.Start(config);
        HostConfig activeConfig = config;
        servermode::DesktopBinding desktop;
        while (running_) {
            if (!desktop.Attach()) {
                LogLine("服务器桌面切换暂未完成，重新启动会话代理：%lu", GetLastError());
                return;
            }
            FrameStream stream;
            status(L"正在初始化画面");
            if (!stream.Initialize(activeConfig)) {
                LogLine("初始化视频链路失败，请检查被控端日志");
                status(L"画面初始化失败");
                for (int i = 0; i < 20 && running_; ++i) Sleep(100);
                continue;
            }
            auto channel = std::make_shared<HostChannel>();
            status(config.reverse ? L"正在连接" : L"等待连接");
            unsigned failures = 0;
            while (running_ && !channel->Open(config, Locator().Current(), running_)) {
                // 连续三次连不上就在网线链路上寻找控制端的当前地址。
                if (config.reverse && ++failures >= 3) Locator().Find(running_);
                for (int i = 0; i < 10 && running_; ++i) Sleep(100);
                channel = std::make_shared<HostChannel>();
            }
            {
                std::lock_guard lock(mutex_);
                channel_ = channel;
                if (!running_) channel->Shutdown();
            }
            if (running_ && channel->Authenticate(config.token, stream.Width(), stream.Height(), stream.Fps())) {
                status(L"已连接");
                LogLine("控制端已通过认证：%s", WideToUtf8(Locator().Current()).c_str());
                if (config.reverse) Locator().Succeeded();
                bool gpuRecovered = false;
                if (Serve(*channel, stream, activeConfig, owner, running_, gpuRecovered) && config.encoder == L"auto") {
                    if (activeConfig.encoder != L"software") {
                        activeConfig.encoder = L"software";
                        LogLine("硬件视频路径运行失败，下一次连接改用 GPU 采集和系统软件编码");
                    } else {
                        activeConfig.capture = L"gdi";
                        LogLine("GPU 兼容路径运行失败，下一次连接使用 GDI；升级通道仍然保留");
                    }
                }
                LogLine("控制连接已断开");
            }
            {
                std::lock_guard lock(mutex_);
                channel_.reset();
            }
            channel->Shutdown();
            for (int i = 0; i < 10 && running_; ++i) Sleep(100);
        }
    }
    HostChannel::CloseListener();
    MFShutdown();
    CoUninitialize();
}
