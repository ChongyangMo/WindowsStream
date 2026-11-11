#include "access.h"
#include "network_menu.h"

void NetworkAccess::Start(HWND window, const std::wstring& ini, ProxyServer& server) {
    window_ = window; ini_ = ini; server_ = &server;
    server_->SetVirtualMachinesAllowed(GetPrivateProfileIntW(L"proxy", L"allow_virtual_machines", 1, ini.c_str()) != 0);
    enabled_ = GetPrivateProfileIntW(L"proxy", L"system_proxy", 1, ini.c_str()) != 0;
    useTunnel_ = false;
    const auto port = GetPrivateProfileIntW(L"proxy", L"local_port", 1080, ini.c_str());
    port_ = port && port <= 65535 ? static_cast<uint16_t>(port) : 0;
    if (!proxy::RestoreSnapshot(PortableDir() + L"\\.windowsstream-proxy.ini")) error_ = true;
}

LRESULT NetworkAccess::MenuState() {
    using namespace network_menu;
    if (!server_) return Unavailable;
    const auto status = error_ ? Failed : !enabled_ ? Off : !lease_.Active() ? Waiting :
        !useTunnel_ || tunnel_.Ready() ? Active : tunnel_.Starting() ? TunnelStarting : TunnelUnavailable;
    return static_cast<LRESULT>(status | (enabled_ ? kEnabled : 0) |
        (server_->VirtualMachinesAllowed() ? kVirtualMachinesAllowed : 0));
}
void NetworkAccess::Tick(bool connected) {
    connected_ = connected;
    if (suspending_) return;
    const auto now = GetTickCount64();
    if (now < retry_) return;
    error_ = false;
    if (lease_.Active() && !lease_.Owns()) {
        tunnel_.Stop();
        // 其他代理工具或用户已接管时不反复覆盖；允许用户再次点开。
        error_ = !lease_.Stop(); enabled_ = false;
        WritePrivateProfileStringW(L"proxy", L"system_proxy", L"0", ini_.c_str());
        LogLine("内网访问已停止，保留外部修改的系统代理设置");
    } else if (enabled_ && connected && server_ && server_->Available()) {
        if (!lease_.Active()) error_ = !lease_.Start(port_);
        if (lease_.Active() && useTunnel_) tunnel_.Start(ini_);
    } else {
        tunnel_.Stop();
        if (lease_.Active()) error_ = !lease_.Stop();
    }
    if (error_) retry_ = now + 10000;
}
bool NetworkAccess::Handle(UINT message, WPARAM wParam) {
    if (message == WM_COMMAND && LOWORD(wParam) == network_menu::kVirtualMachines && server_) {
        const bool allowed = !server_->VirtualMachinesAllowed();
        if (!WritePrivateProfileStringW(L"proxy", L"allow_virtual_machines", allowed ? L"1" : L"0", ini_.c_str())) {
            LogLine("保存虚拟机访问开关失败：%lu", GetLastError()); return true;
        }
        server_->SetVirtualMachinesAllowed(allowed);
        LogLine("允许虚拟机访问已%s", allowed ? "开启" : "关闭");
        return true;
    }
    if (message == WM_COMMAND && LOWORD(wParam) == network_menu::kPing && server_) { ping_.Show(window_, ini_); return true; }
    if (message == WM_COMMAND && LOWORD(wParam) == network_menu::kAccess && server_) {
        const bool desired = !enabled_;
        if (!WritePrivateProfileStringW(L"proxy", L"system_proxy", desired ? L"1" : L"0", ini_.c_str())) {
            LogLine("保存内网访问开关失败：%lu", GetLastError()); return true;
        }
        enabled_ = desired; retry_ = 0; Tick(connected_); return true;
    }
    if (message == WM_QUERYENDSESSION) { suspending_ = true; Stop(); }
    if (message == WM_ENDSESSION && !wParam) { suspending_ = false; retry_ = 0; Tick(connected_); }
    return false;
}
void NetworkAccess::Stop() {
    ping_.Close();
    tunnel_.Stop();
    if (!lease_.Stop()) LogLine("退出时恢复系统代理失败，恢复守护将在进程结束后重试");
}
