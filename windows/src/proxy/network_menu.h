#pragma once
#include <windows.h>

namespace network_menu {
constexpr UINT kQuery = WM_APP + 43;
constexpr UINT kAccess = 41001, kPing = 41002, kVirtualMachines = 41003;

enum Status : UINT_PTR {
    Unavailable, Off, Waiting, Active, Failed, TunnelStarting, TunnelUnavailable
};
constexpr UINT_PTR kStatusMask = 0xff;
constexpr UINT_PTR kEnabled = 0x100, kVirtualMachinesAllowed = 0x200;
}
