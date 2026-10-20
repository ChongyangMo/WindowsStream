#pragma once
#include "system_settings.h"
#include "pac_server.h"

namespace proxy {
class SystemProxy {
public:
    ~SystemProxy() { Stop(); }
    bool Start(uint16_t port);
    bool Stop();
    bool Active() const { return active_; }
    bool Owns() const;
private:
    bool Guard();
    void StopGuard(bool restored);
    SystemSettings applied_;
    PacServer pac_;
    HANDLE stop_ = nullptr, guard_ = nullptr;
    bool active_ = false;
};
// 非守护命令返回 -1；守护进程在父进程消失后恢复该次代理设置。
int RunProxyGuard();
}
