#pragma once
#include "../../src/common.h"

namespace servermode {
// 普通 B 端保持原运行方式；只有明确的服务命令才进入服务器模式。
int Dispatch();
bool SecureAgent();
bool LocalSystem();
int RunAgent();
int RunService();
DWORD InteractiveSession();
bool LaunchAgent(DWORD session, PROCESS_INFORMATION& process);

class DesktopBinding {
public:
    ~DesktopBinding();
    bool Attach();
private:
    HDESK original_ = nullptr, attached_ = nullptr;
};
bool DesktopAccessible();
}
