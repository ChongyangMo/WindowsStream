#pragma once
#include "../../src/common.h"
#include <filesystem>

uint64_t ProcessCreation(HANDLE process);
int RunRecovery(DWORD parent, uint64_t created, HANDLE parentHandle, HANDLE stopHandle, HANDLE uiBeat, HANDLE workerBeat);

class KeepAlive {
public:
    ~KeepAlive() { Stop(); }
    bool Start();
    void Pulse(bool workerProgress);
    void Stop();
private:
    HANDLE stop_ = nullptr, child_ = nullptr, uiBeat_ = nullptr, workerBeat_ = nullptr;
    ULONGLONG nextRetry_ = 0;
};
