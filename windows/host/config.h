#pragma once
#include "../src/common.h"

struct HostConfig {
    std::wstring controllerHost;
    std::wstring bindHost;
    uint16_t port = 7400;
    std::string token;
    bool reverse = true;
    bool clipboard = true;
    int maxWidth = 1920;
    int maxHeight = 1080;
    bool nativeResolution = true;
    int fps = 100;
    int bitrateMbps = 48;
    std::wstring capture = L"auto";
    std::wstring encoder = L"auto";
    bool updatesEnabled = true;
    std::wstring updateHost;
    uint16_t updatePort = 7402;
    unsigned updateSeconds = 15;
    bool autoStart = true, tray = true, startHidden = true;
    bool keepAwake = true, watchdog = true;
    bool idlePulse = true;
    unsigned idleSeconds = 120;
};

bool LoadHostConfig(HostConfig& config, std::wstring& error);
