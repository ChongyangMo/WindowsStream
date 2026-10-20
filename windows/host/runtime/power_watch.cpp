#include "power_watch.h"
#include "../../shared/net_health.h"

void PowerWatch::Start(HWND window) {
    window_ = window;
    Tick();
    SetTimer(window_, kPowerWatchTimer, 2000, nullptr);
}

void PowerWatch::Stop() {
    if (window_) KillTimer(window_, kPowerWatchTimer);
    window_ = nullptr;
}

void PowerWatch::Tick() {
    SYSTEM_POWER_STATUS power{};
    const int ac = GetSystemPowerStatus(&power) && power.ACLineStatus != 255 ? power.ACLineStatus : -1;
    uint64_t mbps = 0;
    const int64_t link = net::WiredLinkUp(&mbps) ? static_cast<int64_t>(mbps) : 0;
    if (ac != ac_ && ac >= 0) {
        if (ac_ >= 0) LogLine(ac ? "电源已切换为交流供电" : "电源已切换为电池供电，电量 %u%%", power.BatteryLifePercent);
        else LogLine("当前供电：%s", ac ? "交流电" : "电池");
    }
    if (link != link_) {
        // 电池供电时网卡节能会降速重协商，期间链路可能长时间断开。
        const char* hint = ac == 0 ? "（当前电池供电，网卡节能会降速或断开链路）" : "";
        if (!link) LogLine("有线网卡链路已断开%s", hint);
        else LogLine("有线网卡链路 %lld Mbps%s", link, link < 100 ? hint : "");
    }
    ac_ = ac; link_ = link;
}
