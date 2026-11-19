#pragma once
#include <windows.h>

class HostExitPassword {
public:
    bool Confirm(HWND owner);
    void Cancel();
private:
    static INT_PTR CALLBACK DialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam);
    HWND dialog_ = nullptr;
};
