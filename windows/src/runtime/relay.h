#pragma once
#include "../common.h"

std::wstring ViewerRelayConfig(const std::wstring& profilePath);

class ViewerRelay {
public:
    ViewerRelay() = default;
    ~ViewerRelay();
    ViewerRelay(const ViewerRelay&) = delete;
    ViewerRelay& operator=(const ViewerRelay&) = delete;

    bool Start(const std::wstring& profilePath, uint16_t& port, std::wstring& error);
private:
    HANDLE job_ = nullptr;
};
