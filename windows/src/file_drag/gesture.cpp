#include "gesture.h"
#include "drop.h"

namespace filedrag {
void Gesture::Connect(HWND viewer, const std::shared_ptr<Connection>& connection) {
    std::lock_guard lock(mutex_);
    viewer_ = viewer;
    peer_ = std::make_shared<Transfer>(connection);
}

void Gesture::Disconnect() {
    std::lock_guard lock(mutex_);
    peer_->Stop();
    peer_.reset();
    offer_.reset();
    available_ = false;
    waiting_ = 0;
}

bool Gesture::Receive(uint8_t type, std::vector<uint8_t>& data) {
    switch (type) {
        case MsgFileDragAvailable: {
            std::lock_guard lock(mutex_);
            if (data.empty()) available_ = true;
            return true;
        }
        case MsgFileDragData:
            // 仅网络线程更换 peer_。
            peer_->Receive(data);
            return true;
        case MsgFileDragOffer: {
            if (data.size() < 4) return true;
            const auto id = ReadU32(data.data());
            std::lock_guard lock(mutex_);
            if (!waiting_ || id != waiting_) return true;
            auto files = ReadOffer(data);
            if (files.empty()) {
                waiting_ = 0;
            } else if (!offer_) {
                offer_ = std::make_shared<Offer>(peer_, id, std::move(files));
                PostMessageW(viewer_, kOfferReady, 0, 0);
            }
            return true;
        }
        default:
            return false;
    }
}

void Gesture::Press() {
    Cancel();
    attempted_ = false;
}

void Gesture::Cancel() {
    std::shared_ptr<Transfer> peer;
    std::shared_ptr<Offer> offer;
    uint32_t id = 0;
    {
        std::lock_guard lock(mutex_);
        peer = peer_;
        offer = std::move(offer_);
        id = std::exchange(waiting_, 0);
    }
    if (!offer && peer && id) {
        Writer close;
        close.u32(id);
        peer->Send(MsgFileDragClose, close);
    }
}

void Gesture::Probe(HWND viewer, int x, int y) {
    if (attempted_ || dragging_ || GetCapture() != viewer) return;
    POINT point{x, y};
    if (!ClientToScreen(viewer, &point) ||
        GetAncestor(WindowFromPoint(point), GA_ROOT) == GetAncestor(viewer, GA_ROOT)) return;
    std::shared_ptr<Transfer> peer;
    uint32_t id;
    {
        std::lock_guard lock(mutex_);
        if (!available_) return;
        if (++next_ == 0) ++next_;
        id = waiting_ = next_;
        peer = peer_;
    }
    attempted_ = true;
    Writer request;
    request.u32(id);
    peer->Send(MsgFileDragProbe, request);
}

std::shared_ptr<Offer> Gesture::Take(HWND viewer) {
    std::shared_ptr<Offer> offer;
    {
        std::lock_guard lock(mutex_);
        if (!offer_) return {};
        offer = std::move(offer_);
        waiting_ = 0;
    }
    if (GetCapture() != viewer || !(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) return {};
    return offer;
}

void Gesture::Drop(HWND viewer, const std::shared_ptr<Offer>& offer) {
    dragging_ = true;
    DropFiles(viewer, offer);
    dragging_ = false;
}
}
