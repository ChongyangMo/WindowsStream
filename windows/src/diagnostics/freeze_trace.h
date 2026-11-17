#pragma once
#include "../common.h"
#include <array>
#include <condition_variable>
#include <thread>

enum class VideoWork : uint16_t { Resize = 1, Decode, Overlay, Render, Present, Clear };

class TraceActivity {
public:
    TraceActivity(std::atomic<uint64_t>& slot, uint16_t operation)
        : slot_(slot), previous_(slot.exchange((GetTickCount64() << 16) | operation)) {}
    ~TraceActivity() { slot_ = previous_; }
    TraceActivity(const TraceActivity&) = delete;
    TraceActivity& operator=(const TraceActivity&) = delete;
private:
    std::atomic<uint64_t>& slot_;
    uint64_t previous_;
};

class FreezeTrace {
public:
    ~FreezeTrace();
    void Start(HWND viewer);
    void Stop();
    void Event(const char* label, int64_t first = 0, int64_t second = 0, int64_t third = 0);
    void Receive(uint8_t type);
    void Sent(uint8_t type, bool ok, bool connected);

    std::atomic<uint64_t> uiWork{0}, sendWork{0}, videoWork{0}, networkWork{0};
    std::atomic<uint64_t> lastUiTimer{0}, lastPacket{0}, lastPong{0}, lastTelemetry{0};
    std::atomic<uint64_t> lastVideo{0}, lastDecode{0}, lastPresent{0};
    std::atomic<uint64_t> session{0}, mouseMoves{0}, mouseSent{0}, buttonsSent{0}, sendsFailed{0}, sendsOffline{0};
    std::atomic<uint64_t> videoPackets{0}, decoded{0}, decodeErrors{0}, presented{0};
    std::atomic<size_t> videoQueue{0};
    std::atomic<HRESULT> presentResult{S_OK};
    std::atomic<int64_t> rttMicros{-1};

private:
    struct Entry { SYSTEMTIME time; const char* label; int64_t first, second, third; };
    void Run(HWND viewer, std::wstring directory);
    std::mutex eventsMutex_, stopMutex_;
    std::array<Entry, 256> events_{};
    size_t eventCount_ = 0;
    std::atomic<unsigned> skippedEvents_{0};
    std::condition_variable wake_;
    bool stopping_ = false;
    std::thread worker_;
};

FreezeTrace& ControllerTrace();
