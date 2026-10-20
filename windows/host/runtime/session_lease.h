#pragma once
#include "../../src/common.h"

struct SessionLeaseLock {
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\WindowsStreamScreenSaverLease");
    bool locked = false;
    SessionLeaseLock() {
        if (mutex) { const auto wait = WaitForSingleObject(mutex, 5000); locked = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED; }
    }
    ~SessionLeaseLock() { if (locked) ReleaseMutex(mutex); if (mutex) CloseHandle(mutex); }
};
