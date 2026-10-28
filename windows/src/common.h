#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

// 协议消息类型，见 PROTOCOL.md。
enum MessageType : uint8_t {
    MsgServerHello = 1,
    MsgClientAuth = 2,
    MsgAuthResult = 3,
    MsgVideo = 10,
    MsgCursor = 11,
    MsgLossless = 12,
    MsgPing = 20,
    MsgPong = 21,
    MsgStats = 22,
    MsgHostInfo = 23,
    MsgTelemetry = 24,
    MsgClipboard = 30,
    MsgMouseMove = 40,
    MsgMouseButton = 41,
    MsgScroll = 42,
    MsgKey = 43,
    MsgReleaseAll = 44,
    MsgViewSize = 45,
    MsgMonitorMode = 46,
    MsgRequestKeyframe = 50,
};

constexpr uint32_t kProtocolVersion = 2;

uint64_t NowMicros();
// 便携模式：exe 旁有 client.ini 或 host.ini 时返回该目录，配置和日志保存在该处。
// 不依赖 %APPDATA%：从某些打包应用里启动的进程，AppData 写入会被系统重定向，与资源管理器启动的进程看到的不一致。
std::wstring PortableDir();
// 每个远程标签页使用自己的配置目录和日志目录。
void SetRuntimeDirectory(const std::wstring& directory);
void LogLine(const char* format, ...);
std::wstring Utf8ToWide(const std::string& text);
std::string WideToUtf8(const std::wstring& text);

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
};

// 视频在窗口中的显示区域：等比缩放居中。画面放得下且只差几个像素时保持 1:1，避免插值发虚。
inline Rect VideoRect(int clientW, int clientH, int frameW, int frameH) {
    if (clientW <= 0 || clientH <= 0 || frameW <= 0 || frameH <= 0) {
        return {};
    }
    double scale = std::min(static_cast<double>(clientW) / frameW, static_cast<double>(clientH) / frameH);
    if (scale >= 1.0 && scale < 1.06) {
        scale = 1.0;
    }
    Rect r;
    r.w = static_cast<int>(frameW * scale + 0.5);
    r.h = static_cast<int>(frameH * scale + 0.5);
    r.x = (clientW - r.w) / 2;
    r.y = (clientH - r.h) / 2;
    return r;
}

// 小端序写入辅助。
struct Writer {
    std::vector<uint8_t> bytes;
    void u8(uint8_t v) { bytes.push_back(v); }
    void u16(uint16_t v) { u8(static_cast<uint8_t>(v)); u8(static_cast<uint8_t>(v >> 8)); }
    void u32(uint32_t v) { u16(static_cast<uint16_t>(v)); u16(static_cast<uint16_t>(v >> 16)); }
    void u64(uint64_t v) { u32(static_cast<uint32_t>(v)); u32(static_cast<uint32_t>(v >> 32)); }
    void append(const void* data, size_t size) {
        if (!size) return;
        auto p = static_cast<const uint8_t*>(data);
        bytes.insert(bytes.end(), p, p + size);
    }
};

inline uint16_t ReadU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
inline uint32_t ReadU32(const uint8_t* p) { return ReadU16(p) | static_cast<uint32_t>(ReadU16(p + 2)) << 16; }
inline uint64_t ReadU64(const uint8_t* p) { return ReadU32(p) | static_cast<uint64_t>(ReadU32(p + 4)) << 32; }
