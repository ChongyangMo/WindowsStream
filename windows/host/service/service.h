#pragma once
#include "../../src/common.h"
#include "../../shared/capture_status.h"

namespace servermode {
// 普通 B 端保持原运行方式；只有明确的服务命令才进入服务器模式。
int Dispatch();
bool SecureAgent();
bool ConsentAgent();
bool ConsentActive();
bool ConsentInput(uint8_t type, const std::vector<uint8_t>& payload);
bool LocalSystem();
int RunAgent(const std::wstring& attentionEvent);
int RunConsentAgent();
int RunService(bool consent = false);
DWORD InteractiveSession();
bool ConsentSession(DWORD session);
bool LaunchAgent(DWORD session, const std::wstring& attentionEvent, PROCESS_INFORMATION& process, bool consent);
HANDLE CreateAttentionEvent(std::wstring& name);
void SendAttention(HANDLE agent);
bool RequestAttention();

class DesktopBinding {
public:
    ~DesktopBinding() = default;
    bool Attach();
};
bool DesktopAccessible();
CaptureStatus QueryCapturePauseStatus();
}
