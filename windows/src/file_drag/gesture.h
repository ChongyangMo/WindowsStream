#pragma once

#include "transfer.h"

namespace filedrag {
constexpr UINT kOfferReady = WM_APP + 8;

class Gesture {
public:
    // 网络线程处理协商与内容回复，窗口线程处理鼠标手势和原生拖放。
    void Connect(HWND viewer, const std::shared_ptr<Connection>& connection);
    void Disconnect();
    bool Receive(uint8_t type, std::vector<uint8_t>& data);
    void Press();
    void Cancel();
    void Probe(HWND viewer, int x, int y);
    std::shared_ptr<Offer> Take(HWND viewer);
    void Drop(HWND viewer, const std::shared_ptr<Offer>& offer);
    bool Dragging() const { return dragging_; }

private:
    std::mutex mutex_;
    std::shared_ptr<Transfer> peer_;
    std::shared_ptr<Offer> offer_;
    HWND viewer_ = nullptr;
    uint32_t next_ = 0;
    uint32_t waiting_ = 0;
    bool available_ = false;
    bool attempted_ = false;
    bool dragging_ = false;
};
}
