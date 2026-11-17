#include "transfer.h"
#include <chrono>

namespace filedrag {
bool Transfer::Send(uint8_t type, const Writer& data) {
    return !stopped_ && connection_->Send(type, data.bytes.data(), data.bytes.size());
}

void Transfer::Stop() {
    std::lock_guard lock(mutex_);
    stopped_ = true;
    ready_.notify_all();
}

void Transfer::Receive(std::vector<uint8_t>& data) {
    if (data.size() < 5 || data.size() > 5 + kReadSize) return;
    std::lock_guard lock(mutex_);
    if (pending_ && pending_ == ReadU32(data.data()) && response_.empty()) {
        response_.swap(data);
        ready_.notify_one();
    }
}

bool Transfer::Read(uint32_t offer, uint16_t index, uint64_t offset, uint32_t size, std::vector<uint8_t>& data) {
    std::lock_guard serial(serial_);
    if (++next_ == 0) ++next_;
    {
        std::lock_guard lock(mutex_);
        pending_ = next_;
        response_.clear();
    }
    Writer request;
    request.u32(offer);
    request.u32(next_);
    request.u16(index);
    request.u64(offset);
    request.u32(size);
    const bool sent = Send(MsgFileDragRead, request);
    std::unique_lock lock(mutex_);
    if (sent) ready_.wait_for(lock, std::chrono::seconds(10), [&] { return stopped_ || !response_.empty(); });
    pending_ = 0;
    if (response_.size() != 5 + size || response_[4] != 1) {
        LogLine("未收到完整的拖拽文件内容");
        return false;
    }
    data.assign(response_.begin() + 5, response_.end());
    return true;
}

Offer::~Offer() {
    Writer close;
    close.u32(id);
    peer->Send(MsgFileDragClose, close);
}
}
