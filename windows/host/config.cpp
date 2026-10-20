#include "config.h"

bool LoadHostConfig(HostConfig& config, std::wstring& error) {
    const auto directory = PortableDir();
    if (directory.empty()) {
        error = L"请将 host.ini 放在被控端程序旁边";
        return false;
    }
    const auto path = directory + L"\\host.ini";
    wchar_t buffer[1024]{};
    GetPrivateProfileStringW(L"connection", L"mode", L"reverse", buffer, 1024, path.c_str());
    if (_wcsicmp(buffer, L"reverse") != 0 && _wcsicmp(buffer, L"listen") != 0) {
        error = L"host.ini 的 mode 只能是 reverse 或 listen";
        return false;
    }
    config.reverse = _wcsicmp(buffer, L"reverse") == 0;
    GetPrivateProfileStringW(L"connection", L"controller_host", L"", buffer, 1024, path.c_str());
    config.controllerHost = buffer;
    config.updatesEnabled = GetPrivateProfileIntW(L"updates", L"enabled", 1, path.c_str()) != 0;
    // 为空时跟随自动发现得到的控制端地址。
    GetPrivateProfileStringW(L"updates", L"host", L"", buffer, 1024, path.c_str());
    config.updateHost = buffer;
    const UINT updatePort = GetPrivateProfileIntW(L"updates", L"port", 7402, path.c_str());
    config.updateSeconds = GetPrivateProfileIntW(L"updates", L"check_seconds", 15, path.c_str());
    if (!updatePort || updatePort > 65535 || config.updateSeconds < 5 || config.updateSeconds > 3600) {
        error = L"请检查升级服务端口和检查间隔"; return false;
    }
    config.updatePort = static_cast<uint16_t>(updatePort);
    GetPrivateProfileStringW(L"connection", L"bind_host", L"0.0.0.0", buffer, 1024, path.c_str());
    config.bindHost = buffer;
    const UINT port = GetPrivateProfileIntW(L"connection", L"port", 7400, path.c_str());
    GetPrivateProfileStringW(L"connection", L"token", L"", buffer, 1024, path.c_str());
    config.token = WideToUtf8(buffer);
    if (port < 1 || port > 65535 || config.token.size() < 32 ||
        (config.reverse && config.controllerHost.empty())) {
        error = L"请检查 host.ini 的连接地址、端口和口令（口令至少 32 个字节）";
        return false;
    }
    config.port = static_cast<uint16_t>(port);
    config.fps = static_cast<int>(GetPrivateProfileIntW(L"video", L"fps", 100, path.c_str()));
    config.bitrateMbps = static_cast<int>(GetPrivateProfileIntW(L"video", L"bitrate_mbps", 48, path.c_str()));
    GetPrivateProfileStringW(L"video", L"capture", L"auto", buffer, 1024, path.c_str());
    config.capture = buffer;
    GetPrivateProfileStringW(L"video", L"encoder", L"auto", buffer, 1024, path.c_str());
    config.encoder = buffer;
    if ((config.capture != L"auto" && config.capture != L"dxgi" && config.capture != L"gdi") ||
        (config.encoder != L"auto" && config.encoder != L"hardware" && config.encoder != L"software")) {
        error = L"采集配置只能是 auto、dxgi、gdi；编码配置只能是 auto、hardware、software";
        return false;
    }
    config.maxWidth = static_cast<int>(GetPrivateProfileIntW(L"video", L"max_width", 1920, path.c_str()));
    config.maxHeight = static_cast<int>(GetPrivateProfileIntW(L"video", L"max_height", 1080, path.c_str()));
    GetPrivateProfileStringW(L"video", L"resolution", L"native", buffer, 1024, path.c_str());
    if (_wcsicmp(buffer, L"native") != 0 && _wcsicmp(buffer, L"limited") != 0) {
        error = L"分辨率配置只能是 native 或 limited"; return false;
    }
    config.nativeResolution = _wcsicmp(buffer, L"native") == 0;
    config.clipboard = GetPrivateProfileIntW(L"permissions", L"clipboard", 1, path.c_str()) != 0;
    config.autoStart = GetPrivateProfileIntW(L"background", L"autostart", 1, path.c_str()) != 0;
    config.tray = GetPrivateProfileIntW(L"background", L"tray", 1, path.c_str()) != 0;
    config.startHidden = GetPrivateProfileIntW(L"background", L"start_hidden", 1, path.c_str()) != 0;
    config.keepAwake = GetPrivateProfileIntW(L"background", L"keep_awake", 1, path.c_str()) != 0;
    config.watchdog = GetPrivateProfileIntW(L"background", L"watchdog", 1, path.c_str()) != 0;
    config.idlePulse = GetPrivateProfileIntW(L"background", L"idle_pulse", 1, path.c_str()) != 0;
    config.idleSeconds = GetPrivateProfileIntW(L"background", L"idle_seconds", 120, path.c_str());
    if (config.idleSeconds < 1 || config.idleSeconds > 3600) { error = L"空闲保活间隔必须在 1 到 3600 秒之间"; return false; }
    if (config.fps < 1 || config.fps > 240 || config.bitrateMbps < 1 || config.bitrateMbps > 100 ||
        config.maxWidth < 320 || config.maxWidth > 3840 || config.maxHeight < 200 || config.maxHeight > 2160) {
        error = L"视频配置超出范围，请检查 host.ini";
        return false;
    }
    return true;
}
