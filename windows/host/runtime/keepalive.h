#pragma once
#include "../../src/common.h"
#include <filesystem>

uint64_t ProcessCreation(HANDLE process);
int RunRecovery(DWORD parent, uint64_t created, HANDLE parentHandle, HANDLE stopHandle);

class KeepAlive {
public:
    ~KeepAlive() { Stop(); }
    bool Start();
    void Stop();
private:
    HANDLE stop_ = nullptr, child_ = nullptr;
};
