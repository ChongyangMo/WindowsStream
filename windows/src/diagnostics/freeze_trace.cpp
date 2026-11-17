#include "freeze_trace.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {
void WriteTime(std::ostream& output, const SYSTEMTIME& time) {
    char text[32]{};
    snprintf(text, sizeof text, "%04u-%02u-%02u %02u:%02u:%02u.%03u", time.wYear, time.wMonth,
             time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    output << text;
}

int64_t Age(ULONGLONG now, uint64_t timestamp) {
    return timestamp ? static_cast<int64_t>(now >= timestamp ? now - timestamp : 0) : -1;
}

void Activity(std::ostream& out, const char* label, ULONGLONG now, uint64_t mark) {
    out << ' ' << label << '=' << (mark & 0xffff) << '/' << Age(now, mark >> 16);
}

void Append(const std::filesystem::path& path, const std::filesystem::path& previous, const std::string& bytes) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (!error && size + bytes.size() > 8 * 1024 * 1024) {
        if (!MoveFileExW(path.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING)) return;
    }
    std::ofstream output(path, std::ios::binary | std::ios::app);
    if (output) output << bytes;
}
}

FreezeTrace& ControllerTrace() {
    static FreezeTrace trace;
    return trace;
}

FreezeTrace::~FreezeTrace() { Stop(); }

void FreezeTrace::Start(HWND viewer) {
    auto directory = PortableDir();
    if (directory.empty()) return;
    try {
        worker_ = std::thread(&FreezeTrace::Run, this, viewer, std::move(directory));
    } catch (const std::system_error&) {
        LogLine("无法启动卡顿监听线程");
    }
}

void FreezeTrace::Stop() {
    if (!worker_.joinable()) return;
    {
        std::lock_guard lock(stopMutex_);
        stopping_ = true;
    }
    wake_.notify_one();
    worker_.join();
}

void FreezeTrace::Event(const char* label, int64_t first, int64_t second, int64_t third) {
    std::unique_lock lock(eventsMutex_, std::try_to_lock);
    if (!lock || eventCount_ == events_.size()) { ++skippedEvents_; return; }
    SYSTEMTIME time{};
    GetLocalTime(&time);
    events_[eventCount_++] = {time, label, first, second, third};
}

void FreezeTrace::Receive(uint8_t type) {
    const auto now = GetTickCount64();
    lastPacket = now;
    if (type == MsgVideo) { lastVideo = now; ++videoPackets; }
    else if (type == MsgPong) lastPong = now;
    else if (type == MsgTelemetry) lastTelemetry = now;
}

void FreezeTrace::Sent(uint8_t type, bool ok, bool connected) {
    if (ok && type == MsgMouseMove) ++mouseSent;
    if (ok && type == MsgMouseButton) ++buttonsSent;
    if (!ok) ++(connected ? sendsFailed : sendsOffline);
    if (type == MsgMouseButton || type == MsgScroll || type == MsgReleaseAll)
        Event("控制消息提交（类型/成功/连接）", type, ok, connected);
}

void FreezeTrace::Run(HWND viewer, std::wstring directory) {
    using namespace std::chrono_literals;
    const auto path = std::filesystem::path(directory) / L"freeze.log";
    const auto previous = path.parent_path() / L"freeze.previous.log";
    const auto pid = GetCurrentProcessId();
    Event("启动卡顿监听（进程/阶段耗时和时间差均为毫秒；收包阶段 1连接 2读取 3分发；视频阶段 1调整 2解码 3图层 4渲染 5显示 6清屏）", pid);
    decltype(events_) batch{};
    for (;;) {
        bool stop;
        {
            std::unique_lock lock(stopMutex_);
            stop = wake_.wait_for(lock, 1s, [this] { return stopping_; });
        }
        size_t count;
        {
            std::lock_guard lock(eventsMutex_);
            count = eventCount_;
            std::copy_n(events_.begin(), count, batch.begin());
            eventCount_ = 0;
        }
        std::ostringstream out;
        for (size_t i = 0; i < count; ++i) {
            const auto& entry = batch[i];
            WriteTime(out, entry.time);
            out << " 进程=" << pid << ' ' << entry.label << ' ' << entry.first << '/' << entry.second << '/' << entry.third << '\n';
        }
        SYSTEMTIME time{};
        GetLocalTime(&time);
        WriteTime(out, time);
        const auto now = GetTickCount64();
        out << " 进程=" << pid << " 会话=" << session.load() << " 可见=" << (IsWindowVisible(viewer) != FALSE)
            << " 窗口计时器距今=" << Age(now, lastUiTimer) << " 收包距今=" << Age(now, lastPacket)
            << " 心跳距今=" << Age(now, lastPong) << " 往返微秒=" << rttMicros.load()
            << " 被控统计距今=" << Age(now, lastTelemetry)
            << " 收画距今=" << Age(now, lastVideo) << " 解码距今=" << Age(now, lastDecode)
            << " 显示距今=" << Age(now, lastPresent);
        Activity(out, "窗口消息", now, uiWork);
        Activity(out, "发送消息", now, sendWork);
        Activity(out, "收包阶段", now, networkWork);
        Activity(out, "视频阶段", now, videoWork);
        out << " 鼠标移动/发送/按键发送=" << mouseMoves.load() << '/' << mouseSent.load() << '/' << buttonsSent.load()
            << " 发送失败/离线=" << sendsFailed.load() << '/' << sendsOffline.load()
            << " 视频包/解码/显示=" << videoPackets.load() << '/' << decoded.load() << '/' << presented.load()
            << " 待处理=" << videoQueue.load() << " 解码错误=" << decodeErrors.load()
            << " 显示结果=0x" << std::hex << static_cast<unsigned>(presentResult.load()) << std::dec
            << " 遗漏事件=" << skippedEvents_.exchange(0) << (stop ? " 停止监听\n" : "\n");
        Append(path, previous, out.str());
        if (stop) break;
    }
}
