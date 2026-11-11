#include "window.h"
#include "../../proxy/network_menu.h"

namespace workspace {
namespace {
const wchar_t* AccessLabel(DWORD_PTR state) {
    using namespace network_menu;
    switch (state & kStatusMask) {
        case Off: return L"内网访问：关闭";
        case Waiting: return L"内网访问：等待连接";
        case Active: return L"内网访问：已开启";
        case Failed: return L"内网访问：失败";
        case TunnelStarting: return L"内网访问：IP 启动中";
        case TunnelUnavailable: return L"内网访问：IP 不可用";
        default: return L"内网访问";
    }
}
}

const Tab* Window::PrimaryTab() const {
    const auto tab = std::find_if(tabs_.begin(), tabs_.end(), [](const Tab& item) { return item.profile.primary; });
    return tab != tabs_.end() ? &*tab : nullptr;
}

void Window::RefreshNetworkMenu() {
    using namespace network_menu;
    const auto tab = PrimaryTab();
    if (!tab) {
        ClearNetworkMenu();
        return;
    }
    if (!networkMenu_) {
        const auto menu = CreateMenu();
        if (!menu) return;
        if (!AppendMenuW(menu, MF_STRING | MF_GRAYED, kAccess, AccessLabel(Unavailable)) ||
            !AppendMenuW(menu, MF_STRING | MF_GRAYED, kPing, L"内网 Ping") ||
            !AppendMenuW(menu, MF_STRING | MF_GRAYED, kVirtualMachines, L"允许虚拟机访问") ||
            !SetMenu(window_, menu)) {
            DestroyMenu(menu);
            return;
        }
        networkMenu_ = menu;
        DrawMenuBar(window_);
    }

    DWORD_PTR state = Unavailable;
    if (tab->view && !SendMessageTimeoutW(tab->view, kQuery, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 25, &state)) return;
    if (state == networkState_) return;
    networkState_ = state;
    const UINT availability = (state & kStatusMask) == Unavailable ? MF_GRAYED : MF_ENABLED;
    ModifyMenuW(networkMenu_, kAccess, MF_BYCOMMAND | MF_STRING | availability |
        ((state & kEnabled) ? MF_CHECKED : MF_UNCHECKED), kAccess, AccessLabel(state));
    EnableMenuItem(networkMenu_, kPing, MF_BYCOMMAND | availability);
    EnableMenuItem(networkMenu_, kVirtualMachines, MF_BYCOMMAND | availability);
    CheckMenuItem(networkMenu_, kVirtualMachines, MF_BYCOMMAND |
        ((state & kVirtualMachinesAllowed) ? MF_CHECKED : MF_UNCHECKED));
    DrawMenuBar(window_);
}

void Window::ClearNetworkMenu() {
    if (!networkMenu_) return;
    SetMenu(window_, nullptr);
    DestroyMenu(networkMenu_);
    networkMenu_ = nullptr;
    networkState_ = network_menu::Unavailable;
}

bool Window::HandleNetworkCommand(UINT command) {
    using namespace network_menu;
    if (command != kAccess && command != kPing && command != kVirtualMachines) return false;
    const auto tab = PrimaryTab();
    if (tab && tab->view) PostMessageW(tab->view, WM_COMMAND, command, 0);
    return true;
}
}
