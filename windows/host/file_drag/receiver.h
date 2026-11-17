#pragma once

#include "../../shared/file_drag.h"
#include <memory>

class HostChannel;

namespace filedrag {
class Receiver {
public:
    explicit Receiver(HostChannel& channel);
    ~Receiver();
    bool Start();
    bool Handle(uint8_t type, const std::vector<uint8_t>& payload);

private:
    struct Batch;
    void Offer(const std::vector<uint8_t>& payload, uint32_t id);
    void Write(const std::vector<uint8_t>& payload);
    void Finish(DWORD error);
    void Reply(uint32_t id, UploadStatus status, uint16_t completed = 0, DWORD error = 0);

    HostChannel& channel_;
    bool enabled_ = false;
    std::unique_ptr<Batch> batch_;
};
}
