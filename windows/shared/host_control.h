#pragma once
#include "../src/common.h"

constexpr UINT kHostShowWindow = WM_APP + 21;
constexpr UINT kHostExitForUpdate = WM_APP + 22;
constexpr UINT kHostExitCommand = 41002;
inline constexpr wchar_t kHostInstanceMutex[] = L"Local\\WindowsStreamHost";

// 恢复与升级共用此锁，避免同时终止或启动同一个被控端。
class HostUpdateLock {
public:
    explicit HostUpdateLock(DWORD timeout) {
        if (mutex_) {
            const auto result = WaitForSingleObject(mutex_, timeout);
            acquired_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
        }
    }
    ~HostUpdateLock() {
        if (acquired_) ReleaseMutex(mutex_);
        if (mutex_) CloseHandle(mutex_);
    }
    HostUpdateLock(const HostUpdateLock&) = delete;
    HostUpdateLock& operator=(const HostUpdateLock&) = delete;
    explicit operator bool() const { return acquired_; }
private:
    HANDLE mutex_ = CreateMutexW(nullptr, FALSE, L"Local\\WindowsStreamHostUpdate");
    bool acquired_ = false;
};
