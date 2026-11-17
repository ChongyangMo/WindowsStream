#pragma once

#include "../net.h"
#include "../../shared/file_drag.h"
#include <condition_variable>
#include <memory>

namespace filedrag {
class Transfer {
public:
    explicit Transfer(std::shared_ptr<Connection> connection) : connection_(std::move(connection)) {}
    bool Send(uint8_t type, const Writer& data);
    void Stop();
    void Receive(std::vector<uint8_t>& data);
    bool Read(uint32_t offer, uint16_t index, uint64_t offset, uint32_t size, std::vector<uint8_t>& data);

private:
    std::shared_ptr<Connection> connection_;
    std::atomic<bool> stopped_{false};
    std::mutex serial_;
    std::mutex mutex_;
    std::condition_variable ready_;
    uint32_t next_ = 0;
    uint32_t pending_ = 0;
    std::vector<uint8_t> response_;
};

struct Offer {
    Offer(std::shared_ptr<Transfer> peer, uint32_t id, std::vector<File> files)
        : peer(std::move(peer)), id(id), files(std::move(files)) {}
    ~Offer();
    Offer(const Offer&) = delete;
    Offer& operator=(const Offer&) = delete;

    std::shared_ptr<Transfer> peer;
    uint32_t id;
    std::vector<File> files;
};
}
