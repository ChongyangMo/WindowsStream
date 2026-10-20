#include "runtime.h"
#include "autorun.h"
#include "../../shared/host_control.h"

bool HostRuntime::Awake(bool enabled) {
    if (enabled) {
        if (!guard_.Start()) { LogLine("启动常亮恢复守护失败，未更改屏保"); return false; }
        if (!awake_.Enable()) { if (!config_.watchdog) guard_.Stop(); return false; }
        SetTimer(window_, 30, 30000, nullptr);
        idle_.Start(window_, config_.idlePulse, config_.idleSeconds);
    } else {
        idle_.Stop(); KillTimer(window_, 30); awake_.Release();
        if (!config_.watchdog) guard_.Stop();
    }
    return true;
}
void HostRuntime::Save(const wchar_t* key, bool enabled) {
    if (!WritePrivateProfileStringW(L"background", key, enabled ? L"1" : L"0", ini_.c_str())) LogLine("保存后台运行配置失败：%lu", GetLastError());
}
void HostRuntime::Start(HWND window, const HostConfig& config, bool background, int show, const std::function<void()>& quit) {
    window_ = window; config_ = config; quit_ = quit;
    ini_ = PortableDir() + L"\\host.ini";
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    if (!SetUserAutoStart(config_.autoStart)) config_.autoStart = false;
    if (config_.keepAwake && !Awake(true)) config_.keepAwake = false;
    if (!config_.keepAwake) RestoreDormantScreenSaver(PortableDir());
    power_.Start(window_);
    if (config_.watchdog && !guard_.Start()) { config_.watchdog = false; LogLine("启动异常退出守护失败：%lu", GetLastError()); }
    const bool hidden = config_.tray && tray_.Add(window_) && (background || config_.startHidden);
    ShowWindow(window_, hidden ? SW_HIDE : show == SW_HIDE ? SW_SHOWNOACTIVATE : show);
    LogLine("托盘常驻：%s；登录自启 %s，保持唤醒 %s，异常恢复 %s", tray_.Added() ? "已启用" : "未启用",
        config_.autoStart ? "开" : "关", config_.keepAwake ? "开" : "关", config_.watchdog ? "开" : "关");
}
void HostRuntime::Stop() { power_.Stop(); idle_.Stop(); KillTimer(window_, 30); awake_.Release(updateHandoff_); guard_.Stop(); tray_.Remove(); }
void HostRuntime::Show() { ShowWindow(window_, SW_RESTORE); SetForegroundWindow(window_); }
void HostRuntime::Status(const std::wstring& status) { status_ = status; tray_.Status(status_); }
void HostRuntime::Exit(const char* reason) {
    if (exiting_) return;
    exiting_ = true;
    LogLine("被控端结束：%s，不触发异常恢复", reason);
    Stop();
    if (quit_) quit_();
}
bool HostRuntime::Handle(UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result) {
    result = 0;
    if (!window_) return false;
    if (taskbarCreated_ && message == taskbarCreated_) {
        if (config_.tray) {
            if (tray_.Add(window_)) tray_.Status(status_);
            else Show();
        }
        return true;
    }
    switch (message) {
        case WM_TIMER:
            if (wParam == 30) { awake_.Refresh(); return true; }
            if (wParam == kIdlePulseTimer) { idle_.Tick(); return true; }
            if (wParam == kPowerWatchTimer) { power_.Tick(); return true; }
            break;
        case WM_SYSCOMMAND:
            if (config_.keepAwake && ((wParam & 0xFFF0) == SC_SCREENSAVE || (wParam & 0xFFF0) == SC_MONITORPOWER)) return true;
            break;
        case WM_CLOSE:
            if (tray_.Added()) { ShowWindow(window_, SW_HIDE); LogLine("窗口已隐藏到托盘，被控端继续运行"); }
            else Exit("关闭窗口");
            return true;
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED && tray_.Added()) { ShowWindow(window_, SW_HIDE); return true; }
            break;
        case kHostShowWindow: Show(); return true;
        case kHostExitForUpdate: updateHandoff_ = true; Exit("正在升级"); return true;
        case kTrayCallback: {
            const auto event = LOWORD(lParam);
            if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP) tray_.Menu(config_.autoStart, config_.keepAwake);
            else if (event == NIN_SELECT || event == NIN_KEYSELECT || event == WM_LBUTTONDBLCLK) Show();
            return true;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case 1: if (tray_.Added()) ShowWindow(window_, SW_HIDE); else Exit("退出按钮"); return true;
                case 2: case kHostExitCommand: Exit("用户主动退出"); return true;
                case kTrayShow: Show(); return true;
                case kTrayAutoStart:
                    if (SetUserAutoStart(!config_.autoStart)) { config_.autoStart = !config_.autoStart; Save(L"autostart", config_.autoStart); }
                    return true;
                case kTrayKeepAwake:
                    if (Awake(!config_.keepAwake)) { config_.keepAwake = !config_.keepAwake; Save(L"keep_awake", config_.keepAwake); }
                    return true;
            }
            break;
        case WM_QUERYENDSESSION:
            idle_.Stop(); KillTimer(window_, 30); awake_.Release(); guard_.Stop(); result = TRUE; return true;
        case WM_ENDSESSION:
            if (wParam) Exit("系统注销或关机");
            else {
                if (config_.watchdog && !guard_.Start()) LogLine("取消注销后恢复守护失败");
                if (config_.keepAwake) Awake(true);
            }
            return true;
        case WM_POWERBROADCAST:
            if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) {
                if (config_.keepAwake) Awake(true);
                LogLine("系统从休眠恢复，保留自动重连和后台升级");
            }
            result = TRUE; return true;
    }
    return false;
}
