#pragma once
#include "../video.h"
#include "../../shared/telemetry.h"

struct DisplayMetrics {
    int width = 0, height = 0, refreshHz = 0, viewWidth = 0, viewHeight = 0;
};

class StreamDiagnostics {
public:
    void Begin(int width, int height, int fps);
    void End();
    void Received(size_t bytes, bool video);
    void Remote(const std::vector<uint8_t>& payload);
    void Tick(const VideoStats& stats, int64_t rttUs, const DisplayMetrics& display);
    std::wstring Title();
private:
    std::mutex mutex_;
    bool connected_ = false, remoteValid_ = false;
    int width_ = 0, height_ = 0, fpsCap_ = 0;
    uint64_t tickUs_ = 0, logUs_ = 0, remoteUs_ = 0, bytes_ = 0, packets_ = 0, session_ = 0;
    HostTelemetry remote_;
    std::wstring title_;
};
