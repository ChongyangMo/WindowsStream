#pragma once
#include "../common.h"

namespace proxy {
struct SystemSettings {
    DWORD flags = 0;
    std::wstring server, bypass, script;
    bool operator==(const SystemSettings&) const = default;
};
struct ProxySnapshot {
    DWORD process = 0;
    uint64_t created = 0;
    SystemSettings original, applied;
};
struct SettingsLock {
    SettingsLock();
    ~SettingsLock();
    HANDLE mutex = nullptr;
    bool locked = false;
};
uint64_t Created(HANDLE process);
bool ReadSystemSettings(SystemSettings& settings);
bool WriteSystemSettings(const SystemSettings& settings);
bool SaveSnapshot(const std::wstring& path, const ProxySnapshot& state);
bool LoadSnapshot(const std::wstring& path, ProxySnapshot& state);
bool RestoreSnapshot(const std::wstring& path, DWORD owner = 0, uint64_t created = 0);
}
