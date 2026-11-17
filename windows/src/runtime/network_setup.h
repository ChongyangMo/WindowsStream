#pragma once
#include "../common.h"

namespace network_setup {
void AddTrayMenu(HMENU tray);
bool HandleTrayCommand(HWND owner, UINT command);
int Dispatch();
}
