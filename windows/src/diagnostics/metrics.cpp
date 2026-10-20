#include "metrics.h"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace {
double Average(uint64_t sum, uint32_t count) { return count ? sum / 1000.0 / count : -1; }
std::string Number(double value, int precision = 2) {
    if (value < 0) return "null";
    std::ostringstream out;
    out.imbue(std::locale::classic()); out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}
std::wstring Milliseconds(double value) { return value < 0 ? L"—" : Utf8ToWide(Number(value, 1)) + L"ms"; }
double RemoteMs(uint32_t value) { return value == kUnknownMetric ? -1 : value / 1000.0; }
const wchar_t* Pipeline(uint8_t value) {
    return value == 1 ? L"GPU 硬编" : value == 2 ? L"GPU 软编" : value == 3 ? L"GDI 软编" : L"等待诊断";
}
void Append(const std::string& text) {
    const auto directory = PortableDir();
    if (directory.empty()) return;
    const auto path = std::filesystem::path(directory) / L"performance.jsonl";
    std::error_code error;
    if (std::filesystem::file_size(path, error) > 4 * 1024 * 1024 && !error) {
        const auto previous = std::filesystem::path(directory) / L"performance.previous.jsonl";
        std::filesystem::remove(previous, error);
        error.clear(); std::filesystem::rename(path, previous, error);
    }
    std::ofstream file(path, std::ios::binary | std::ios::app);
    if (file) file << text << '\n';
}
}

void StreamDiagnostics::Begin(int width, int height, int fps) {
    std::lock_guard lock(mutex_);
    connected_ = true; remoteValid_ = false;
    width_ = width; height_ = height; fpsCap_ = fps;
    tickUs_ = session_ = NowMicros(); logUs_ = remoteUs_ = bytes_ = packets_ = 0;
    remote_ = {}; title_.clear();
}
void StreamDiagnostics::End() {
    std::lock_guard lock(mutex_);
    connected_ = false; remoteValid_ = false; title_.clear();
}
void StreamDiagnostics::Received(size_t bytes, bool video) {
    std::lock_guard lock(mutex_);
    if (!connected_) return;
    bytes_ += bytes; if (video) ++packets_;
}
void StreamDiagnostics::Remote(const std::vector<uint8_t>& payload) {
    HostTelemetry sample;
    if (!DecodeTelemetry(payload, sample)) { LogLine("被控端结构化诊断格式无效"); return; }
    std::lock_guard lock(mutex_);
    if (!connected_) return;
    remote_ = sample; remoteValid_ = true; remoteUs_ = NowMicros();
}
std::wstring StreamDiagnostics::Title() { std::lock_guard lock(mutex_); return title_; }

void StreamDiagnostics::Tick(const VideoStats& stats, int64_t rttUs, const DisplayMetrics& display) {
    std::lock_guard lock(mutex_);
    if (!connected_) return;
    const uint64_t now = NowMicros();
    const double seconds = (now - tickUs_) / 1e6;
    if (seconds <= 0) return;
    const double receivedFps = packets_ / seconds, presentFps = stats.presented / seconds;
    const double mbps = bytes_ * 8 / seconds / 1e6;
    const double latency = Average(stats.latencySumMicros, stats.latencySamples);
    const double decode = Average(stats.decodeMicros, stats.decodeBatches);
    const double render = Average(stats.renderMicros, stats.renderSamples);
    const double queue = Average(stats.queueMicros, stats.queueSamples);
    const bool fresh = remoteValid_ && now - remoteUs_ <= 6000000;
    const auto& h = remote_;
    title_ = std::to_wstring(width_) + L"×" + std::to_wstring(height_) +
        L" · 接收 " + Utf8ToWide(Number(receivedFps, 1)) + L" / 显示 " + Utf8ToWide(Number(presentFps, 1)) +
        L" fps · 上限 " + std::to_wstring(fpsCap_) + L" · RTT " + Milliseconds(rttUs < 0 ? -1 : rttUs / 1000.0) +
        L" · 画面 " + Milliseconds(latency) + L" · " + Utf8ToWide(Number(mbps, 1)) + L"Mbps · " +
        Pipeline(fresh ? h.pipeline : 0);
    if (now - logUs_ >= 2000000) {
        const auto fit = VideoRect(display.viewWidth, display.viewHeight, width_, height_);
        const double scale = width_ ? fit.w / static_cast<double>(width_) : -1;
        SYSTEMTIME time{}; GetLocalTime(&time);
        char timestamp[40]{};
        snprintf(timestamp, sizeof timestamp, "%04u-%02u-%02uT%02u:%02u:%02u.%03u", time.wYear, time.wMonth,
                 time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
        std::ostringstream json;
        json.imbue(std::locale::classic());
        json << "{\"time\":\"" << timestamp << "\",\"session_us\":" << session_ << ",\"sample_seconds\":" << Number(seconds)
            << ",\"width\":" << width_ << ",\"height\":" << height_ << ",\"fps_cap\":" << fpsCap_
            << ",\"receive_fps\":" << Number(receivedFps) << ",\"decode_fps\":" << Number(stats.decoded / seconds)
            << ",\"present_fps\":" << Number(presentFps) << ",\"mbps\":" << Number(mbps)
            << ",\"rtt_ms\":" << Number(rttUs < 0 ? -1 : rttUs / 1000.0) << ",\"capture_to_present_ms\":" << Number(latency)
            << ",\"latency_samples\":" << stats.latencySamples << ",\"decode_ms\":" << Number(decode)
            << ",\"render_ms\":" << Number(render) << ",\"client_queue_ms\":" << Number(queue)
            << ",\"client_queue_max\":" << stats.maxQueue << ",\"decode_errors\":" << stats.decodeErrors
            << ",\"view_width\":" << display.viewWidth << ",\"view_height\":" << display.viewHeight
            << ",\"display_width\":" << display.width << ",\"display_height\":" << display.height
            << ",\"display_hz\":" << (display.refreshHz ? std::to_string(display.refreshHz) : "null")
            << ",\"display_scale\":" << Number(scale) << ",\"host_fresh\":" << (fresh ? "true" : "false")
            << ",\"host_sample_age_ms\":" << (remoteValid_ ? std::to_string((now - remoteUs_) / 1000) : "null")
            << ",\"host_source_width\":" << (fresh ? std::to_string(h.sourceWidth) : "null")
            << ",\"host_source_height\":" << (fresh ? std::to_string(h.sourceHeight) : "null")
            << ",\"host_refresh_hz\":" << (fresh && h.refreshHz ? std::to_string(h.refreshHz) : "null")
            << ",\"host_pipeline\":" << (fresh ? std::to_string(h.pipeline) : "null")
            << ",\"host_sample_ms\":" << (fresh && h.sampleMillis ? std::to_string(h.sampleMillis) : "null")
            << ",\"host_capture_fps\":" << Number(fresh && h.sampleMillis ? h.captureMilliFps / 1000.0 : -1)
            << ",\"host_output_fps\":" << Number(fresh && h.sampleMillis ? h.outputMilliFps / 1000.0 : -1)
            << ",\"host_capture_ms\":" << Number(fresh ? RemoteMs(h.captureUs) : -1)
            << ",\"host_submit_ms\":" << Number(fresh ? RemoteMs(h.submitUs) : -1)
            << ",\"host_output_age_ms\":" << Number(fresh ? RemoteMs(h.outputAgeUs) : -1)
            << ",\"host_send_ms\":" << Number(fresh ? RemoteMs(h.sendUs) : -1)
            << ",\"host_pending\":" << (fresh ? std::to_string(h.pending) : "null")
            << ",\"host_skipped\":" << (fresh ? std::to_string(h.skipped) : "null") << "}";
        Append(json.str());
        const auto source = fresh ? std::to_wstring(h.sourceWidth) + L"×" + std::to_wstring(h.sourceHeight) +
            L"/" + (h.refreshHz ? std::to_wstring(h.refreshHz) + L"Hz" : L"—") : L"—";
        LogLine("诊断：%s；排队 %s，解码 %s，渲染 %s；源屏 %s，采集 %s，编码输出 %s，待编码 %s；显示缩放 %.2f",
            WideToUtf8(title_).c_str(), WideToUtf8(Milliseconds(queue)).c_str(), WideToUtf8(Milliseconds(decode)).c_str(),
            WideToUtf8(Milliseconds(render)).c_str(), WideToUtf8(source).c_str(),
            WideToUtf8(Milliseconds(fresh ? RemoteMs(h.captureUs) : -1)).c_str(),
            WideToUtf8(Milliseconds(fresh ? RemoteMs(h.outputAgeUs) : -1)).c_str(), fresh ? std::to_string(h.pending).c_str() : "—", scale);
        logUs_ = now;
    }
    tickUs_ = now; bytes_ = packets_ = 0;
}
