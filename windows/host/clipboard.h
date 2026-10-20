#pragma once
#include "../src/common.h"

class ClipboardBridge {
public:
    explicit ClipboardBridge(HWND owner) : owner_(owner) {}
    bool Read(std::string& text);
    bool Apply(const std::vector<uint8_t>& utf8);
private:
    HWND owner_ = nullptr;
    DWORD sequence_ = 0;
    std::mutex mutex_;
};
