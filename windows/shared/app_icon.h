#pragma once
#include <windows.h>
#include "../resources/resource.h"

// LR_SHARED 图标由系统管理，窗口和托盘可以安全复用。
inline HICON LoadAppIcon(bool smallIcon = false) {
    return static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
        MAKEINTRESOURCEW(IDI_WINDOWSSTREAM), IMAGE_ICON,
        GetSystemMetrics(smallIcon ? SM_CXSMICON : SM_CXICON),
        GetSystemMetrics(smallIcon ? SM_CYSMICON : SM_CYICON), LR_SHARED));
}
