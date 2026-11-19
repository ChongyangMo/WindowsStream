#include "runtime.h"
#include "autorun.h"
#include "keepalive_task.h"
#include "../../shared/host_control.h"

bool HostRuntime::Awake(bool enabled) {
    if (enabled) {
        if (!guard_.Start()) { LogLine("启动常亮恢复守护失败，未更改屏保"); return false; }
        if (!awake_.Enable(config_.disableManualLock)) { if (!config_.watchdog) guard_.Stop(); return false; }
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
void HostRuntime::Start(HWND window, const HostConfig& config, bool background, int show, const std::function<void()>& quit,
                        const std::function<unsigned()>& workerProgress) {
    window_ = window; config_ = config; quit_ = quit; workerProgress_ = workerProgress;
    ini_ = PortableDir() + L"\\host.ini";
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    const bool guardReady = !config_.watchdog || guard_.Start();
    if (config_.manageAutoStart && !SetUserAutoStart(config_.autoStart, config_.relayAutoStart)) config_.autoStart = false;
    if (config_.keepAwake && !Awake(true)) config_.keepAwake = false;
    if (!config_.keepAwake) RestoreDormantScreenSaver(PortableDir());
    power_.Start(window_);
    const bool hidden = config_.tray && tray_.Add(window_) && background;
    ShowWindow(window_, hidden ? SW_HIDE : show == SW_HIDE ? SW_SHOWNOACTIVATE : show);
    StartScheduledKeepAlive(PortableDir(), config_.elevated);
    LogLine("托盘常驻：%s；登录自启 %s，保持唤醒 %s，异常恢复 %s", tray_.Added() ? "已启用" : "未启用",
        config_.manageAutoStart ? (config_.autoStart ? "开" : "关") : "由外部管理",
        config_.keepAwake ? "开" : "关", config_.watchdog ? (guardReady ? "开" : "等待重试") : "关");
}
void HostRuntime::Stop() { guard_.Stop(); power_.Stop(); idle_.Stop(); KillTimer(window_, 30); awake_.Release(updateHandoff_); tray_.Remove(); }
void HostRuntime::WatchdogTick() {
    if (endingSession_ || (!config_.watchdog && !config_.keepAwake)) return;
    const auto current = workerProgress_();
    guard_.Pulse(current != lastWorkerProgress_);
    lastWorkerProgress_ = current;
}
void HostRuntime::Show() { ShowWindow(window_, SW_RESTORE); SetForegroundWindow(window_); }
void HostRuntime::Status(const std::wstring& status) { status_ = status; tray_.Status(status_); }
void HostRuntime::RequestExit(const char* reason) {
    if (!exiting_ && !endingSession_ && exitPassword_.Confirm(window_) && !endingSession_) Exit(reason);
}
void HostRuntime::Exit(const char* reason) {
    if (exiting_) return;
    exiting_ = true;
    exitPassword_.Cancel();
    if (!updateHandoff_) PauseScheduledKeepAlive(PortableDir());
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
            if (wParam == kPowerWatchTimer) { WatchdogTick(); power_.Tick(); return true; }
            break;
        case WM_SYSCOMMAND:
            if (config_.keepAwake && ((wParam & 0xFFF0) == SC_SCREENSAVE || (wParam & 0xFFF0) == SC_MONITORPOWER)) return true;
            break;
        case WM_CLOSE:
            if (tray_.Added()) { ShowWindow(window_, SW_HIDE); LogLine("窗口已隐藏到托盘，被控端继续运行"); }
            else RequestExit("关闭窗口");
            return true;
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED && tray_.Added()) { ShowWindow(window_, SW_HIDE); return true; }
            break;
        case kHostShowWindow: Show(); return true;
        case kHostExitForUpdate: updateHandoff_ = true; Exit("正在升级"); return true;
        case kTrayCallback: {
            const auto event = LOWORD(lParam);
            if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP)
                tray_.Menu(config_.manageAutoStart, config_.autoStart, config_.keepAwake);
            else if (event == NIN_SELECT || event == NIN_KEYSELECT || event == WM_LBUTTONDBLCLK) Show();
            return true;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case 1:
                    if (tray_.Added()) ShowWindow(window_, SW_HIDE);
                    else RequestExit("用户主动退出");
                    return true;
                case 2: RequestExit("用户主动退出"); return true;
                case kHostExitCommand:
                    if (HIWORD(wParam) == kHostReplacementNotification && !HostUpdateLock(0))
                        Exit("正在替换被控端");
                    else RequestExit("用户主动退出");
                    return true;
                case kTrayShow: Show(); return true;
                case kTrayAutoStart:
                    if (config_.manageAutoStart) {
                        if (SaveUserAutoStart(!config_.autoStart, config_.relayAutoStart, config_.autoStartIni))
                            config_.autoStart = !config_.autoStart;
                        else MessageBoxW(window_, L"无法保存开机自启动设置，请检查程序目录和 Windows 启动权限。",
                                         L"WindowsStream", MB_OK | MB_ICONERROR);
                    }
                    return true;
                case kTrayKeepAwake:
                    if (Awake(!config_.keepAwake)) { config_.keepAwake = !config_.keepAwake; Save(L"keep_awake", config_.keepAwake); }
                    return true;
            }
            break;
        case WM_QUERYENDSESSION:
            endingSession_ = true;
            guard_.Stop(); idle_.Stop(); KillTimer(window_, 30); awake_.Release(); result = TRUE; return true;
        case WM_ENDSESSION:
            if (wParam) Exit("系统注销或关机");
            else {
                endingSession_ = false;
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
