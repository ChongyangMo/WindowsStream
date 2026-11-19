#pragma once

#include <cstdint>
#include <string_view>

enum class CaptureStatus : uint8_t { Ready, Unavailable, Locked, DesktopUnavailable, Consent };

inline constexpr wchar_t kConsentInputLabel[] = L"UAC 加密输入";

struct CaptureStatusText {
    CaptureStatus status;
    std::string_view message;
    const wchar_t* label;
};

inline constexpr CaptureStatusText kCaptureStatusText[]{
    {CaptureStatus::Ready, "画面采集已恢复", L""},
    {CaptureStatus::Unavailable, "画面采集暂不可用，控制连接保持", L"采集暂停"},
    {CaptureStatus::Locked, "被控端 Windows 已锁屏，等待解锁", L"已锁屏"},
    {CaptureStatus::DesktopUnavailable, "被控端桌面不可访问，可能有 UAC 提示", L"桌面不可访问"},
};
